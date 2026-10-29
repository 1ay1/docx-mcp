// Field results: what Word shows after Update Field (F9). Parses field
// instructions and their switches, evaluates nested fields inside out, and
// writes results back keeping the result run's formatting. Page fields take
// their numbers from a real layout (page_map) when asked for.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/fsio.hpp"
#include "util/util.hpp"
#include "wml/docops.hpp"
#include "wml/edit.hpp"
#include "wml/fields_internal.hpp"
#include "wml/render.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

// ── instruction parsing ──────────────────────────────────────────────────

namespace {

struct Token {
    std::string text;
    bool quoted = false;
    bool sw = false;  // a \x switch
};

// Split a field instruction into words, quoted strings and switches. Word
// allows \" inside quotes and \\ for a backslash.
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> v;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        if (std::isspace(c)) { i++; continue; }
        Token t;
        if (c == '"' || s.compare(i, 3, "\xE2\x80\x9C") == 0) {  // straight or curly opening quote
            t.quoted = true;
            i += c == '"' ? 1 : 3;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) { t.text += s[i + 1]; i += 2; continue; }
                if (s[i] == '"') { i++; break; }
                if (s.compare(i, 3, "\xE2\x80\x9D") == 0) { i += 3; break; }
                t.text += s[i++];
            }
        } else if (c == '\\' && i + 1 < s.size() && !std::isspace((unsigned char)s[i + 1])) {
            t.sw = true;
            t.text = s.substr(i, 2);
            i += 2;
            // \@, \#, \* etc. take the next token as argument, handled by the caller
        } else {
            while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '"') t.text += s[i++];
        }
        v.push_back(std::move(t));
    }
    return v;
}

struct Instr {
    std::string name;                 // upper case
    std::vector<Token> args;          // positional arguments
    std::map<std::string, std::vector<std::string>> sw;  // "\@" -> args (repeatable: \* Upper \* MERGEFORMAT)
    bool has(const char* k) const { return sw.count(k) > 0; }
    std::string get(const char* k) const {
        auto it = sw.find(k);
        return it == sw.end() || it->second.empty() ? "" : it->second.front();
    }
    std::string arg(std::size_t i) const { return i < args.size() ? args[i].text : ""; }
};

// Switches that take an argument.
bool takes_arg(const std::string& sw, const std::string& field) {
    if (sw == "\\@" || sw == "\\#" || sw == "\\*") return true;
    if (field == "SEQ") return sw == "\\r" || sw == "\\s";
    if (field == "MERGEFIELD") return sw == "\\b" || sw == "\\f";
    if (field == "STYLEREF") return false;
    if (field == "TOC") return sw == "\\o" || sw == "\\c" || sw == "\\b" || sw == "\\t" || sw == "\\f" || sw == "\\l" || sw == "\\n" || sw == "\\p" || sw == "\\s" || sw == "\\d";
    if (field == "INDEX") return sw != "\\h" ? sw == "\\c" || sw == "\\e" || sw == "\\b" || sw == "\\f" || sw == "\\g" || sw == "\\k" || sw == "\\l" || sw == "\\p" || sw == "\\s" || sw == "\\d" || sw == "\\h" : true;
    if (field == "DOCPROPERTY" || field == "INFO" || field == "DOCVARIABLE") return false;
    if (field == "SET" || field == "ASK" || field == "FILLIN") return sw == "\\d";
    if (field == "XE") return sw == "\\f" || sw == "\\r" || sw == "\\t";
    return false;
}

Instr parse(const std::string& code) {
    Instr in;
    auto toks = tokenize(code);
    if (toks.empty()) return in;
    std::size_t i = 0;
    if (toks[0].text == "=") in.name = "=";
    else if (!toks[0].text.empty() && toks[0].text[0] == '=') {
        in.name = "=";
        toks[0].text = toks[0].text.substr(1);
        i = 0;
        // fallthrough: the rest of the first token is the start of the expression
        for (; i < toks.size(); i++) {
            if (toks[i].sw) break;
            in.args.push_back(toks[i]);
        }
    }
    if (in.name.empty()) in.name = util::upper(toks[0].text);
    if (in.name == "=" && in.args.empty()) i = 1;
    else if (in.name != "=") i = 1;
    for (; i < toks.size(); i++) {
        auto& t = toks[i];
        if (t.sw) {
            std::string k = t.text;
            if (k.size() == 2 && k[1] != '@' && k[1] != '#' && k[1] != '*') k[1] = (char)std::tolower((unsigned char)k[1]);
            auto& slot = in.sw[k];
            if (takes_arg(k, in.name) && i + 1 < toks.size() && !toks[i + 1].sw) slot.push_back(toks[++i].text);
            continue;
        }
        in.args.push_back(t);
    }
    return in;
}

// ── formatting ───────────────────────────────────────────────────────────

// The whole string is a number (no trailing text), else NaN.
double strict_number(std::string_view s0) {
    std::string s = util::trim(s0);
    if (s.empty()) return NAN;
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return NAN;
    return d;
}

const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

std::string pad(int v, int w) {
    std::string s = std::to_string(v);
    while ((int)s.size() < w) s = "0" + s;
    return s;
}

std::string upper_first(std::string s) {
    for (std::size_t i = 0; i < s.size(); i++)
        if (std::isalpha((unsigned char)s[i])) { s[i] = (char)std::toupper((unsigned char)s[i]); break; }
    return s;
}

std::string title_case(std::string s) {
    bool start = true;
    for (auto& c : s) {
        if (std::isalpha((unsigned char)c)) {
            if (start) c = (char)std::toupper((unsigned char)c);
            start = false;
        } else start = std::isspace((unsigned char)c) || c == '-';
    }
    return s;
}

}  // namespace

std::string format_date(const std::tm& t, const std::string& pic) {
    std::string o;
    std::size_t i = 0;
    auto run = [&](char c) {
        std::size_t n = 0;
        while (i + n < pic.size() && pic[i + n] == c) n++;
        return n;
    };
    while (i < pic.size()) {
        char c = pic[i];
        if (c == '\'') {  // literal text
            auto e = pic.find('\'', i + 1);
            o += pic.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
            i = e == std::string::npos ? pic.size() : e + 1;
            continue;
        }
        std::size_t n = run(c);
        int h12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
        switch (c) {
            case 'M':
                if (n >= 4) o += kMonths[t.tm_mon];
                else if (n == 3) o += std::string(kMonths[t.tm_mon]).substr(0, 3);
                else o += pad(t.tm_mon + 1, (int)n);
                break;
            case 'd': case 'D':
                if (n >= 4) o += kDays[t.tm_wday];
                else if (n == 3) o += std::string(kDays[t.tm_wday]).substr(0, 3);
                else o += pad(t.tm_mday, (int)n);
                break;
            case 'y': case 'Y':
                if (n >= 3) o += std::to_string(t.tm_year + 1900);
                else o += pad((t.tm_year + 1900) % 100, 2);
                break;
            case 'h': o += pad(h12, (int)std::min<std::size_t>(n, 2)); break;
            case 'H': o += pad(t.tm_hour, (int)std::min<std::size_t>(n, 2)); break;
            case 'm': o += pad(t.tm_min, (int)std::min<std::size_t>(n, 2)); break;
            case 's': case 'S': o += pad(t.tm_sec, (int)std::min<std::size_t>(n, 2)); break;
            case 'a': case 'A':
                if (util::iequals(pic.substr(i, 5), "am/pm")) {
                    std::string ap = t.tm_hour < 12 ? "AM" : "PM";
                    if (c == 'a') ap = util::lower(ap);
                    o += ap;
                    i += 5;
                    continue;
                }
                o += std::string(n, c);
                break;
            default: o += std::string(n, c);
        }
        i += n;
    }
    return o;
}

std::string format_numeric(double v, const std::string& pic_in) {
    // sections: positive;negative;zero
    auto parts = util::split(pic_in, ';');
    std::string pic = parts[0];
    bool neg = v < 0;
    if (neg && parts.size() > 1 && !parts[1].empty()) { pic = parts[1]; v = -v; neg = false; }
    else if (v == 0 && parts.size() > 2 && !parts[2].empty()) pic = parts[2];
    // locate the numeric part: the run of 0 # , . x characters
    std::size_t a = pic.find_first_of("0#x");
    if (a == std::string::npos) return pic;
    std::size_t b = a;
    while (b < pic.size() && std::strchr("0#x,.", pic[b])) b++;
    // a trailing '.' with no digits after belongs to the literal text
    while (b > a && pic[b - 1] == ',') b--;
    std::string num = pic.substr(a, b - a), pre = pic.substr(0, a), post = pic.substr(b);
    auto dot = num.find('.');
    std::string ip = dot == std::string::npos ? num : num.substr(0, dot);
    std::string fp = dot == std::string::npos ? "" : num.substr(dot + 1);
    bool group = ip.find(',') != std::string::npos;
    int decimals = 0, min_dec = 0;
    for (char c : fp) {
        if (c == '0' || c == '#' || c == 'x') decimals++;
        if (c == '0') min_dec = decimals;
    }
    int min_int = 0;
    for (char c : ip) if (c == '0') min_int++;
    double av = std::fabs(v);
    // x drops digits to its left (Word truncation)
    double scale = std::pow(10.0, decimals);
    double r = std::round(av * scale) / scale;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, r);
    std::string s = buf;
    std::string is = s, fs;
    if (auto d = s.find('.'); d != std::string::npos) { is = s.substr(0, d); fs = s.substr(d + 1); }
    while ((int)fs.size() > min_dec && !fs.empty() && fs.back() == '0') fs.pop_back();
    if (is == "0" && min_int == 0) is.clear();
    while ((int)is.size() < min_int) is = "0" + is;
    if (group) {
        std::string g;
        int k = 0;
        for (int i = (int)is.size() - 1; i >= 0; i--) {
            g.insert(g.begin(), is[i]);
            if (++k % 3 == 0 && i > 0) g.insert(g.begin(), ',');
        }
        is = g;
    }
    std::string out = is;
    if (!fs.empty()) out += "." + fs;
    if (out.empty()) out = "0";
    // a sign position in the picture (- or +) else a leading minus
    std::string sign = neg ? "-" : "";
    if (!pre.empty() && (pre.back() == '-' || pre.back() == '+')) {
        sign = neg ? "-" : (pre.back() == '+' ? "+" : (r == 0 ? "" : " "));
        pre.pop_back();
    }
    return pre + sign + out + post;
}

// ── evaluation ───────────────────────────────────────────────────────────

namespace {

std::string apply_format(std::string v, const Instr& in) {
    auto it = in.sw.find("\\*");
    if (it == in.sw.end()) return v;
    for (auto& f0 : it->second) {
        std::string f = util::lower(f0);
        if (f == "mergeformat" || f == "charformat") continue;
        if (f == "upper") { for (auto& c : v) c = (char)std::toupper((unsigned char)c); continue; }
        if (f == "lower") { for (auto& c : v) c = (char)std::tolower((unsigned char)c); continue; }
        if (f == "firstcap") { v = upper_first(v); continue; }
        if (f == "caps") { v = title_case(v); continue; }
        // numeric formats need a number
        double num = strict_number(v);
        if (std::isnan(num)) continue;
        int n = (int)std::lround(num);
        if (f == "arabic") v = std::to_string(n);
        else if (f == "roman") v = format_number(n, f0 == "ROMAN" || f0 == "Roman" ? "upperRoman" : "lowerRoman");
        else if (f == "alphabetic") v = format_number(n, f0 == "ALPHABETIC" ? "upperLetter" : "lowerLetter");
        else if (f == "ordinal") v = format_number(n, "ordinal");
        else if (f == "ordtext") v = util::lower(format_number(n, "ordinalText"));
        else if (f == "cardtext") v = util::lower(detail::number_words(n));
        else if (f == "hex") v = format_number(n, "hex");
        else if (f == "dollartext") {
            int cents = (int)std::lround((num - std::floor(num)) * 100);
            v = detail::number_words((int)std::floor(num)) + " and " + pad(cents, 2) + "/100";
        } else if (f == "arabicdash") v = "- " + std::to_string(n) + " -";
    }
    return v;
}

// Arithmetic for = fields: + - * / ^ % comparisons, functions, bookmarks as numbers.
struct Expr {
    std::string s;
    std::size_t i = 0;
    std::function<std::optional<double>(const std::string&)> name;  // bookmark / ABOVE etc
    std::function<std::vector<double>(const std::string&)> range;   // ABOVE, LEFT, ...
    bool bad = false;
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    bool eat(const char* t) {
        ws();
        if (s.compare(i, std::strlen(t), t) == 0) { i += std::strlen(t); return true; }
        return false;
    }
    double cmp() {
        double a = add();
        for (;;) {
            if (eat("<=")) a = a <= add();
            else if (eat(">=")) a = a >= add();
            else if (eat("<>")) a = a != add();
            else if (eat("<")) a = a < add();
            else if (eat(">")) a = a > add();
            else if (eat("=")) a = a == add();
            else return a;
        }
    }
    double add() {
        double a = mul();
        for (;;) {
            if (eat("+")) a += mul();
            else if (eat("-")) a -= mul();
            else return a;
        }
    }
    double mul() {
        double a = pw();
        for (;;) {
            if (eat("*")) a *= pw();
            else if (eat("/")) { double b = pw(); if (b == 0) { bad = true; return 0; } a /= b; }
            else return a;
        }
    }
    double pw() {
        double a = unary();
        if (eat("^")) a = std::pow(a, pw());
        return a;
    }
    double unary() {
        if (eat("-")) return -unary();
        if (eat("+")) return unary();
        double v = atom();
        if (eat("%")) v /= 100;
        return v;
    }
    std::vector<double> args() {
        std::vector<double> v;
        if (eat(")")) return v;
        for (;;) {
            ws();
            std::size_t save = i;
            std::string word;
            while (i < s.size() && (std::isalpha((unsigned char)s[i]))) word += s[i++];
            std::string up = util::upper(word);
            ws();
            if ((up == "ABOVE" || up == "BELOW" || up == "LEFT" || up == "RIGHT") && (i >= s.size() || s[i] == ',' || s[i] == ')')) {
                auto r = range(up);
                v.insert(v.end(), r.begin(), r.end());
            } else {
                i = save;
                v.push_back(cmp());
            }
            if (eat(",")) continue;
            if (eat(")")) return v;
            bad = true;
            return v;
        }
    }
    double atom() {
        ws();
        if (eat("(")) {
            double v = cmp();
            if (!eat(")")) bad = true;
            return v;
        }
        std::size_t st = i;
        if (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' ||
                                    (s[i] == ',' && i + 3 < s.size() + 0 && std::isdigit((unsigned char)s[i + 1]) &&
                                     std::isdigit((unsigned char)s[i + 2]) && std::isdigit((unsigned char)s[i + 3]) &&
                                     (i + 4 >= s.size() || !std::isdigit((unsigned char)s[i + 4])))))
                i++;
            std::string n = s.substr(st, i - st);
            n.erase(std::remove(n.begin(), n.end(), ','), n.end());
            return util::to_double(n);
        }
        std::string w;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) w += s[i++];
        if (w.empty()) { bad = true; i = s.size(); return 0; }
        std::string up = util::upper(w);
        if (eat("(")) {
            auto a = args();
            if (up == "SUM") { double t = 0; for (double x : a) t += x; return t; }
            if (up == "AVERAGE") { double t = 0; for (double x : a) t += x; return a.empty() ? 0 : t / a.size(); }
            if (up == "COUNT") return (double)a.size();
            if (up == "PRODUCT") { double t = 1; for (double x : a) t *= x; return t; }
            if (up == "MAX") return a.empty() ? 0 : *std::max_element(a.begin(), a.end());
            if (up == "MIN") return a.empty() ? 0 : *std::min_element(a.begin(), a.end());
            if (up == "ABS") return a.empty() ? 0 : std::fabs(a[0]);
            if (up == "INT") return a.empty() ? 0 : std::trunc(a[0]);
            if (up == "MOD") return a.size() < 2 || a[1] == 0 ? (bad = true, 0) : std::fmod(a[0], a[1]);
            if (up == "ROUND") {
                if (a.size() < 2) return a.empty() ? 0 : std::round(a[0]);
                double p = std::pow(10.0, a[1]);
                return std::round(a[0] * p) / p;
            }
            if (up == "SIGN") return a.empty() ? 0 : (a[0] > 0) - (a[0] < 0);
            if (up == "AND") return a.size() == 2 && a[0] != 0 && a[1] != 0;
            if (up == "OR") return a.size() == 2 && (a[0] != 0 || a[1] != 0);
            if (up == "NOT") return a.size() == 1 && a[0] == 0;
            if (up == "IF") return a.size() == 3 ? (a[0] != 0 ? a[1] : a[2]) : (bad = true, 0);
            if (up == "DEFINED") return a.size() == 1;
            if (up == "TRUE") return 1;
            if (up == "FALSE") return 0;
            bad = true;
            return 0;
        }
        if (up == "TRUE") return 1;
        if (up == "FALSE") return 0;
        // a table cell reference (A1) or a bookmark name
        if (auto v = name(w)) return *v;
        bad = true;
        return 0;
    }
};

std::string number_text(double v) {
    if (std::fabs(v - std::round(v)) < 1e-9) return std::to_string((long long)std::llround(v));
    char b[64];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

// Word's COMPARE / IF comparison: numbers compare as numbers, text with ? and * wildcards when = / <>.
bool wild(const std::string& p, const std::string& s, std::size_t i = 0, std::size_t j = 0) {
    if (i == p.size()) return j == s.size();
    if (p[i] == '*') {
        for (std::size_t k = j; k <= s.size(); k++) if (wild(p, s, i + 1, k)) return true;
        return false;
    }
    if (j < s.size() && (p[i] == '?' || std::tolower((unsigned char)p[i]) == std::tolower((unsigned char)s[j]))) return wild(p, s, i + 1, j + 1);
    return false;
}

bool compare(const std::string& a, const std::string& op, const std::string& b, bool b_quoted) {
    double x = strict_number(a), y = strict_number(b);
    bool num = !std::isnan(x) && !std::isnan(y);
    if (num) {
        if (op == "=") return x == y;
        if (op == "<>") return x != y;
        if (op == "<") return x < y;
        if (op == ">") return x > y;
        if (op == "<=") return x <= y;
        if (op == ">=") return x >= y;
        return false;
    }
    int c = a.compare(b);
    if (op == "=") return b_quoted && (b.find_first_of("*?") != std::string::npos) ? wild(b, a) : a == b;
    if (op == "<>") return b_quoted && (b.find_first_of("*?") != std::string::npos) ? !wild(b, a) : a != b;
    if (op == "<") return c < 0;
    if (op == ">") return c > 0;
    if (op == "<=") return c <= 0;
    if (op == ">=") return c >= 0;
    return false;
}

// IF / COMPARE / SKIPIF / NEXTIF condition: expr op expr. Tokens may have the
// operator glued on (a=b) or separate (a = b).
std::optional<bool> condition(const std::vector<Token>& args, std::size_t& used) {
    static const char* ops[] = {"<=", ">=", "<>", "=", "<", ">"};
    if (args.empty()) return std::nullopt;
    // glued: one token "a=b"
    if (!args[0].quoted) {
        for (auto op : ops) {
            auto k = args[0].text.find(op);
            if (k != std::string::npos && k > 0) {
                std::string a = args[0].text.substr(0, k), b = args[0].text.substr(k + std::strlen(op));
                if (b.empty() && args.size() > 1) { used = 2; return compare(a, op, args[1].text, args[1].quoted); }
                used = 1;
                return compare(a, op, b, false);
            }
        }
    }
    if (args.size() >= 3) {
        for (auto op : ops)
            if (args[1].text == op) { used = 3; return compare(args[0].text, op, args[2].text, args[2].quoted); }
    }
    if (args.size() >= 2 && !args[1].quoted) {
        for (auto op : ops) {
            if (util::starts_with(args[1].text, op) && args[1].text.size() > std::strlen(op)) {
                used = 2;
                return compare(args[0].text, op, args[1].text.substr(std::strlen(op)), false);
            }
        }
    }
    return std::nullopt;
}

std::string trim_para(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

}  // namespace

// ── the engine ───────────────────────────────────────────────────────────

namespace {

struct Field {
    pugi::xml_node begin, sep, end;  // complex field chars (sep may be null)
    pugi::xml_node simple;           // or a w:fldSimple
    std::string part;
    int depth = 0;
    int anchor = -1;                 // index into field_anchors(body) when in the body
    std::vector<int> kids;           // nested fields (indices)
    int parent = -1;
};

// Every field of one story, nested structure intact, in document order.
std::vector<Field> scan(pugi::xml_node root, const std::string& part) {
    std::vector<Field> v;
    std::vector<int> stack;
    std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
        for (auto c : n.children()) {
            if (c.type() != pugi::node_element) continue;
            std::string_view q = c.name();
            if (q == "mc:Fallback") continue;
            if (q == "w:fldChar") {
                std::string t = xml::attr(c, "w:fldCharType");
                if (t == "begin") {
                    Field f;
                    f.begin = c;
                    f.part = part;
                    f.parent = stack.empty() ? -1 : stack.back();
                    f.depth = (int)stack.size();
                    if (f.parent >= 0) v[f.parent].kids.push_back((int)v.size());
                    stack.push_back((int)v.size());
                    v.push_back(f);
                } else if (t == "separate" && !stack.empty()) {
                    if (!v[stack.back()].sep) v[stack.back()].sep = c;
                } else if (t == "end" && !stack.empty()) {
                    v[stack.back()].end = c;
                    stack.pop_back();
                }
                continue;
            }
            if (q == "w:fldSimple") {
                Field f;
                f.simple = c;
                f.part = part;
                f.parent = stack.empty() ? -1 : stack.back();
                f.depth = (int)stack.size();
                if (f.parent >= 0) v[f.parent].kids.push_back((int)v.size());
                v.push_back(f);
                stack.push_back((int)v.size() - 1);
                walk(c);
                stack.pop_back();
                continue;
            }
            walk(c);
        }
    };
    walk(root);
    // drop unterminated fields
    for (auto& f : v)
        if (f.begin && !f.end) f.begin = {};
    return v;
}

// Node after n in document order within root (descending into children).
pugi::xml_node next_in(pugi::xml_node n, pugi::xml_node root) {
    if (n.first_child()) return n.first_child();
    while (n && n != root) {
        if (n.next_sibling()) return n.next_sibling();
        n = n.parent();
    }
    return {};
}

bool before(pugi::xml_node a, pugi::xml_node b) {
    // document order: compare paths from the root
    std::vector<pugi::xml_node> pa, pb;
    for (auto x = a; x; x = x.parent()) pa.push_back(x);
    for (auto x = b; x; x = x.parent()) pb.push_back(x);
    std::reverse(pa.begin(), pa.end());
    std::reverse(pb.begin(), pb.end());
    std::size_t i = 0;
    while (i < pa.size() && i < pb.size() && pa[i] == pb[i]) i++;
    if (i == pa.size()) return true;   // a is an ancestor of b
    if (i == pb.size()) return false;
    for (auto s = pa[i]; s; s = s.next_sibling())
        if (s == pb[i]) return true;
    return false;
}

class Engine {
public:
    Engine(Document& d, const FieldUpdate& u, const PageMap* pm) : d_(d), u_(u), pm_(pm) {}

    int run() {
        int n = 0;
        // the body first (SEQ numbering, SET bookmarks), then the other stories
        auto parts = d_.story_parts();
        for (auto& part : parts) {
            if (!d_.pkg().has(part)) continue;
            auto root = d_.pkg().xml(part).document_element();
            bool body = part == d_.main();
            fields_ = scan(root, part);
            if (body && pm_) {
                auto anchors = field_anchors(d_.body());
                std::map<pugi::xml_node_struct*, int> at;
                for (std::size_t i = 0; i < anchors.size(); i++) at[anchors[i].internal_object()] = (int)i;
                for (auto& f : fields_) {
                    auto key = (f.begin ? f.begin : f.simple).internal_object();
                    if (auto it = at.find(key); it != at.end()) f.anchor = it->second;
                }
            }
            seq_.clear();
            body_ = body;
            for (std::size_t i = 0; i < fields_.size(); i++) {
                if (fields_[i].parent >= 0) continue;  // nested ones are evaluated through their parent
                if (u_.only && !(fields_[i].begin == u_.only || fields_[i].simple == u_.only)) continue;
                if (update((int)i)) n++;
            }
        }
        return n;
    }

private:
    Document& d_;
    const FieldUpdate& u_;
    const PageMap* pm_;
    std::vector<Field> fields_;
    std::map<std::string, int> seq_;
    std::map<std::string, std::string> set_;  // SET bookmarks
    bool body_ = true;
    std::vector<BookmarkInfo> bms_;
    bool bms_loaded_ = false;

    const std::vector<BookmarkInfo>& bookmarks() {
        if (!bms_loaded_) { bms_ = list_bookmarks(d_); bms_loaded_ = true; }
        return bms_;
    }

    // Instruction text with nested field results spliced in.
    std::string code_of(int fi) {
        auto& f = fields_[fi];
        if (f.simple) return xml::attr(f.simple, "w:instr");
        std::string s;
        auto stop = f.sep ? f.sep : f.end;
        // walk from begin to sep collecting instrText and nested field values
        auto root = xml::root_of(f.begin);
        std::size_t kid = 0;
        for (auto n = next_in(f.begin, root); n && n != stop; ) {
            if (kid < f.kids.size() && fields_[f.kids[kid]].begin == n) {
                auto& k = fields_[f.kids[kid]];
                s += value_of(f.kids[kid]);
                n = k.end;
                kid++;
                n = next_in(n, root);
                continue;
            }
            if (kid < f.kids.size() && fields_[f.kids[kid]].simple == n) {
                s += value_of(f.kids[kid]);
                // skip its subtree
                auto x = n;
                while (x && x != root && !x.next_sibling()) x = x.parent();
                n = x && x != root ? x.next_sibling() : pugi::xml_node();
                kid++;
                continue;
            }
            if (xml::name_is(n, "w:instrText") || xml::name_is(n, "w:delInstrText")) {
                if (!xml::ancestor(n, "w:del") && !xml::name_is(n, "w:delInstrText")) s += n.text().get();
            }
            n = next_in(n, root);
        }
        return s;
    }

    // The value of a nested field (evaluated, else its current result).
    std::string value_of(int fi) {
        auto code = util::trim(code_of(fi));
        if (auto v = eval(fi, code)) return *v;
        return current_result(fi);
    }

    std::string current_result(int fi) {
        auto& f = fields_[fi];
        std::string s;
        if (f.simple) {
            for (auto t : xml::descendants(f.simple, "w:t")) s += t.text().get();
            return s;
        }
        if (!f.sep) return s;
        auto root = xml::root_of(f.begin);
        for (auto n = next_in(f.sep, root); n && n != f.end; n = next_in(n, root))
            if (xml::name_is(n, "w:t") && !xml::ancestor(n, "w:del")) s += n.text().get();
        return s;
    }

    pugi::xml_node para_of(int fi) {
        auto& f = fields_[fi];
        return xml::ancestor(f.begin ? f.begin : f.simple, "w:p");
    }

    int page_of(int fi) {
        if (!pm_ || !body_) return 0;
        int a = fields_[fi].anchor;
        if (a >= 0 && a < (int)pm_->fields.size()) return pm_->fields[a];
        return 0;
    }

    // Section of a body node: index into sections(d).
    pugi::xml_node section_of(pugi::xml_node n) {
        auto p = xml::ancestor(n, "w:body") ? n : pugi::xml_node();
        if (!p) return {};
        // the first paragraph at or after n carrying a sectPr, else the body's
        auto top = n;
        while (top.parent() && !xml::name_is(top.parent(), "w:body")) top = top.parent();
        for (auto b = top; b; b = b.next_sibling())
            if (auto s = b.child("w:pPr").child("w:sectPr")) return s;
        return d_.body().child("w:sectPr");
    }

    std::string page_format(pugi::xml_node sect) { return xml::attr(sect.child("w:pgNumType"), "w:fmt", "decimal"); }

    // Page number as the section numbers it (restart + format).
    std::string page_label(int phys, pugi::xml_node at_sect) {
        if (!pm_) return "";
        // walk sections in order; each section starts on the page of its first paragraph
        auto ss = sections(d_);
        auto ps = paragraphs(d_.body(), false);
        // first paragraph index of each section
        std::vector<int> start_page(ss.size(), 1);
        std::size_t si = 0;
        bool new_sec = true;
        for (std::size_t i = 0; i < ps.size() && si < ss.size(); i++) {
            if (new_sec) { start_page[si] = i < pm_->paras.size() ? pm_->paras[i] : 1; new_sec = false; }
            if (ps[i].child("w:pPr").child("w:sectPr") == ss[si]) { si++; new_sec = true; }
        }
        // which section holds page phys: the last that starts at or before it
        int sec = 0;
        if (at_sect) {
            for (std::size_t k = 0; k < ss.size(); k++) if (ss[k] == at_sect) sec = (int)k;
        } else {
            for (std::size_t k = 0; k < ss.size(); k++) if (start_page[k] <= phys) sec = (int)k;
        }
        // number = continue from earlier sections unless restarted
        int num = phys;
        for (int k = sec; k >= 0; k--) {
            auto st = ss[k].child("w:pgNumType").attribute("w:start");
            if (st) { num = st.as_int() + (phys - start_page[k]); break; }
        }
        return format_number(num, page_format(ss[sec]));
    }

    std::optional<std::string> bookmark_text(const std::string& name) {
        if (auto it = set_.find(util::lower(name)); it != set_.end()) return it->second;
        for (auto& b : bookmarks())
            if (util::iequals(b.name, name)) return b.text;
        return std::nullopt;
    }

    std::optional<std::string> bookmark_para(const std::string& name) {
        for (auto& b : bookmarks())
            if (util::iequals(b.name, name)) return b.para;
        return std::nullopt;
    }

    std::optional<double> bookmark_number(const std::string& name) {
        auto t = bookmark_text(name);
        if (!t) return std::nullopt;
        std::string s = util::trim(*t);
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ',' || c == '$'; }), s.end());
        double v = strict_number(s);
        if (std::isnan(v)) return std::nullopt;
        return v;
    }

    // Table cell values for = fields inside a table.
    std::vector<double> cell_range(int fi, const std::string& dir) {
        std::vector<double> v;
        auto tc = xml::ancestor(fields_[fi].begin ? fields_[fi].begin : fields_[fi].simple, "w:tc");
        if (!tc) return v;
        auto tr = tc.parent();
        auto tbl = tr.parent();
        int col = 0;
        for (auto c = tr.child("w:tc"); c && c != tc; c = c.next_sibling("w:tc")) col++;
        auto num = [&](pugi::xml_node cell) -> std::optional<double> {
            std::string s;
            for (auto p : paragraphs(cell)) s += analyze(p).utf8();
            s = util::trim(s);
            s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ',' || c == '$' || c == '%'; }), s.end());
            if (s.empty()) return std::nullopt;
            double x = strict_number(s);
            if (std::isnan(x)) return std::nullopt;
            return x;
        };
        if (dir == "LEFT" || dir == "RIGHT") {
            int k = 0;
            for (auto c = tr.child("w:tc"); c; c = c.next_sibling("w:tc"), k++)
                if ((dir == "LEFT" && k < col) || (dir == "RIGHT" && k > col))
                    if (auto x = num(c)) v.push_back(*x);
        } else {
            bool past = false;
            for (auto r = tbl.child("w:tr"); r; r = r.next_sibling("w:tr")) {
                if (r == tr) { past = true; continue; }
                if ((dir == "ABOVE" && !past) || (dir == "BELOW" && past)) {
                    int k = 0;
                    for (auto c = r.child("w:tc"); c; c = c.next_sibling("w:tc"), k++)
                        if (k == col) { if (auto x = num(c)) v.push_back(*x); break; }
                }
            }
        }
        return v;
    }

    std::optional<double> cell_ref(int fi, const std::string& ref) {
        // A1-style: letters = column, digits = row
        std::size_t k = 0;
        int col = 0;
        while (k < ref.size() && std::isalpha((unsigned char)ref[k])) col = col * 26 + (std::toupper((unsigned char)ref[k++]) - 'A' + 1);
        if (!col || k == ref.size()) return std::nullopt;
        int row = util::to_int(ref.substr(k));
        for (char c : ref.substr(k)) if (!std::isdigit((unsigned char)c)) return std::nullopt;
        auto tc = xml::ancestor(fields_[fi].begin ? fields_[fi].begin : fields_[fi].simple, "w:tc");
        if (!tc) return std::nullopt;
        auto tbl = tc.parent().parent();
        auto c = cell(tbl, row - 1, col - 1);
        if (!c) return std::nullopt;
        std::string s;
        for (auto p : paragraphs(c)) s += analyze(p).utf8();
        s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == ',' || ch == '$' || ch == '%' || std::isspace((unsigned char)ch); }), s.end());
        double x = strict_number(s);
        if (std::isnan(x)) return std::nullopt;
        return x;
    }

    const Json* record() {
        if (!u_.merge || !u_.merge->rows || !u_.merge->rows->is_array()) return nullptr;
        if (u_.merge->row >= u_.merge->rows->size()) return nullptr;
        return &(*u_.merge->rows)[u_.merge->row];
    }

    std::optional<std::string> merge_value(const std::string& name) {
        auto* r = record();
        if (!r) return std::nullopt;
        if (!r->is_object()) return std::nullopt;
        for (auto& [k, v] : r->items()) {
            if (!util::iequals(k, name)) continue;
            if (v.is_string()) return v.get<std::string>();
            if (v.is_null()) return std::string();
            if (v.is_number_integer()) return std::to_string(v.get<long long>());
            if (v.is_number()) return number_text(v.get<double>());
            if (v.is_boolean()) return std::string(v.get<bool>() ? "TRUE" : "FALSE");
            return v.dump();
        }
        if (std::find(u_.merge->missing.begin(), u_.merge->missing.end(), name) == u_.merge->missing.end())
            u_.merge->missing.push_back(name);
        return std::string();
    }

    std::string doc_property(const std::string& name) {
        std::string k = util::lower(name);
        auto core = core_properties(d_);
        static const std::map<std::string, std::string> alias = {
            {"title", "title"}, {"subject", "subject"}, {"author", "creator"}, {"keywords", "keywords"},
            {"comments", "description"}, {"category", "category"}, {"lastsavedby", "last_modified_by"},
            {"revisionnumber", "revision"}, {"createtime", "created"}, {"lastsavetime", "modified"},
            {"lastprinted", "last_printed"}, {"contentstatus", "content_status"}, {"manager", ""}, {"company", ""}};
        if (auto it = alias.find(k); it != alias.end() && !it->second.empty() && core.contains(it->second))
            return core[it->second].get<std::string>();
        auto app = app_properties(d_);
        static const std::map<std::string, std::string> app_alias = {
            {"company", "Company"}, {"manager", "Manager"}, {"template", "Template"}, {"pages", "Pages"},
            {"words", "Words"}, {"characters", "Characters"}, {"lines", "Lines"}, {"paragraphs", "Paragraphs"},
            {"application", "Application"}, {"hyperlinkbase", "HyperlinkBase"}};
        if (auto it = app_alias.find(k); it != app_alias.end() && app.contains(it->second)) return app[it->second].get<std::string>();
        auto custom = custom_properties(d_);
        for (auto& [ck, cv] : custom.items())
            if (util::iequals(ck, name)) return cv.is_string() ? cv.get<std::string>() : number_text(cv.is_number() ? cv.get<double>() : 0);
        return "";
    }

    std::tm date_of(const std::string& iso) {
        std::tm t{};
        if (iso.size() >= 10) {
            t.tm_year = util::to_int(iso.substr(0, 4)) - 1900;
            t.tm_mon = util::to_int(iso.substr(5, 2)) - 1;
            t.tm_mday = util::to_int(iso.substr(8, 2));
            if (iso.size() >= 19) {
                t.tm_hour = util::to_int(iso.substr(11, 2));
                t.tm_min = util::to_int(iso.substr(14, 2));
                t.tm_sec = util::to_int(iso.substr(17, 2));
            }
            std::tm c = t;
            std::mktime(&c);
            t.tm_wday = c.tm_wday;
        }
        return t;
    }

    std::tm now() {
        std::time_t tt = std::time(nullptr);
        std::tm t{};
#if defined(_WIN32)
        localtime_s(&t, &tt);
#else
        localtime_r(&tt, &t);
#endif
        return t;
    }

    std::string styleref(int fi, const Instr& in) {
        std::string want = in.arg(0);
        bool by_level = !want.empty() && std::all_of(want.begin(), want.end(), [](char c) { return std::isdigit((unsigned char)c); });
        auto matches = [&](pugi::xml_node p) {
            std::string sid = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
            if (by_level) return detail::outline_level(d_, p) == util::to_int(want);
            if (sid.empty()) return util::iequals(want, "Normal");
            if (util::iequals(sid, want)) return true;
            auto st = find_style(d_, want);
            return st && util::iequals(xml::attr(st, "w:styleId"), sid);
        };
        auto text = [&](pugi::xml_node p) {
            if (in.has("\\n") || in.has("\\r") || in.has("\\w")) return list_label(d_, p);
            return analyze(p).utf8();
        };
        auto here = para_of(fi);
        auto ps = paragraphs(d_.body(), false);
        if (body_ && here) {
            // search backwards from here, then forwards (Word's order in body text)
            int idx = -1;
            for (std::size_t i = 0; i < ps.size(); i++) if (ps[i] == here) idx = (int)i;
            if (idx >= 0) {
                for (int i = idx; i >= 0; i--) if (matches(ps[i])) return text(ps[i]);
                for (std::size_t i = idx; i < ps.size(); i++) if (matches(ps[i])) return text(ps[i]);
            }
            return "";
        }
        // in a header/footer: the first match on the page (needs layout), else the first in the document
        for (auto p : ps) if (matches(p)) return text(p);
        return "";
    }

public:
    std::optional<std::string> eval(int fi, const std::string& code) {
        Instr in = parse(code);
        const std::string& f = in.name;
        std::optional<std::string> r;
        if (f == "PAGE") {
            if (!pm_ || !body_) return std::nullopt;
            int pg = page_of(fi);
            if (!pg) return std::nullopt;
            r = page_label(pg, {});
        } else if (f == "NUMPAGES") {
            if (!pm_) return std::nullopt;
            r = std::to_string(pm_->total);
        } else if (f == "SECTIONPAGES") {
            if (!pm_) return std::nullopt;
            if (!body_) return std::nullopt;
            return std::nullopt;
        } else if (f == "SECTION") {
            if (!body_) return std::nullopt;
            auto s = section_of(para_of(fi));
            auto ss = sections(d_);
            for (std::size_t k = 0; k < ss.size(); k++) if (ss[k] == s) r = std::to_string(k + 1);
        } else if (f == "PAGEREF") {
            if (!pm_) return std::nullopt;
            auto it = pm_->bookmark.find(in.arg(0));
            if (it == pm_->bookmark.end()) {
                for (auto& [k, v] : pm_->bookmark) if (util::iequals(k, in.arg(0))) { r = std::to_string(v); break; }
                if (!r) return std::string("Error! Bookmark not defined.");
            }
            int pg = it != pm_->bookmark.end() ? it->second : util::to_int(*r);
            if (in.has("\\p")) {
                int here = page_of(fi);
                if (here == pg) r = "above";
                else r = std::string("on page ") + std::to_string(pg);
            } else {
                r = page_label(pg, {});
            }
        } else if (f == "SEQ") {
            std::string id = in.arg(0);
            if (id.empty()) return std::nullopt;
            int& n = seq_[id];
            if (in.has("\\r")) n = util::to_int(in.get("\\r"));
            else if (in.has("\\c")) {}
            else n++;
            // \s n: restart after each heading of level n
            if (in.has("\\h")) r = "";
            else r = std::to_string(n);
            if (!in.has("\\*")) {}
        } else if (f == "REF" || (!f.empty() && f != "=" && bookmark_text(in.name) && in.args.empty() && f != "IF")) {
            std::string name = f == "REF" ? in.arg(0) : in.name;
            auto t = bookmark_text(name);
            if (!t) return std::string("Error! Reference source not found.");
            r = *t;
            if (in.has("\\n") || in.has("\\r") || in.has("\\w")) {
                auto pid = bookmark_para(name);
                std::string lbl;
                if (pid && !pid->empty()) {
                    try { lbl = list_label(d_, find_para(d_, *pid).p); } catch (...) {}
                }
                while (!lbl.empty() && (lbl.back() == '.' || lbl.back() == ')' || lbl.back() == ' ')) lbl.pop_back();
                // \n / \r / \w with a SEQ-numbered caption: the number inside the bookmark
                if (lbl.empty()) {
                    std::string digits;
                    for (char c : *t) if (std::isdigit((unsigned char)c)) digits += c; else if (!digits.empty()) break;
                    lbl = digits;
                }
                r = lbl;
            }
            if (in.has("\\p") && pm_) {
                // relative position: above / below
                auto pid = bookmark_para(name);
                auto here = para_of(fi);
                if (pid && here) {
                    try {
                        auto target = find_para(d_, *pid).p;
                        *r += before(target, here) ? " above" : " below";
                    } catch (...) {}
                }
            }
        } else if (f == "NOTEREF") {
            // the note number the bookmark's note reference has
            auto name = in.arg(0);
            std::optional<std::string> got;
            for (auto bs : xml::descendants(d_.body(), "w:bookmarkStart")) {
                if (!util::iequals(xml::attr(bs, "w:name"), name)) continue;
                int fn = 0, en = 0;
                for (auto x : xml::descendants(d_.body(), "w:footnoteReference")) { fn++; if (before(bs, x)) { got = std::to_string(fn); break; } }
                (void)en;
                break;
            }
            if (!got) return std::nullopt;
            r = got;
        } else if (f == "STYLEREF") {
            r = styleref(fi, in);
        } else if (f == "DATE" || f == "TIME" || f == "CREATEDATE" || f == "SAVEDATE" || f == "PRINTDATE") {
            std::tm t{};
            auto core = core_properties(d_);
            if (f == "CREATEDATE") { if (!core.contains("created")) return std::nullopt; t = date_of(core["created"]); }
            else if (f == "SAVEDATE") { if (!core.contains("modified")) return std::nullopt; t = date_of(core["modified"]); }
            else if (f == "PRINTDATE") { if (!core.contains("last_printed")) return std::string("0/0/0000 0:00:00 AM"); t = date_of(core["last_printed"]); }
            else t = now();
            std::string pic = in.get("\\@");
            if (pic.empty()) pic = f == "TIME" ? "h:mm am/pm" : (f == "DATE" ? "M/d/yyyy" : "M/d/yyyy h:mm:ss am/pm");
            r = format_date(t, pic);
        } else if (f == "AUTHOR" || f == "TITLE" || f == "SUBJECT" || f == "KEYWORDS" || f == "COMMENTS" || f == "LASTSAVEDBY" ||
                   f == "REVNUM") {
            static const std::map<std::string, std::string> key = {{"AUTHOR", "creator"}, {"TITLE", "title"}, {"SUBJECT", "subject"},
                                                                   {"KEYWORDS", "keywords"}, {"COMMENTS", "description"},
                                                                   {"LASTSAVEDBY", "last_modified_by"}, {"REVNUM", "revision"}};
            auto core = core_properties(d_);
            std::string k = key.at(f);
            r = core.contains(k) ? core[k].get<std::string>() : "";
        } else if (f == "DOCPROPERTY") {
            r = doc_property(in.arg(0));
        } else if (f == "INFO") {
            r = doc_property(in.arg(0));
        } else if (f == "DOCVARIABLE") {
            auto vars = document_variables(d_);
            std::string name = in.arg(0);
            r = "";
            for (auto& [k, v] : vars.items()) if (util::iequals(k, name)) r = v.get<std::string>();
            if (r->empty() && !vars.contains(name)) r = "Error! No document variable supplied.";
        } else if (f == "FILENAME") {
            std::string p = u_.filename;
            if (p.empty()) return std::nullopt;
            if (!in.has("\\p")) {
                auto k = p.find_last_of("/\\");
                if (k != std::string::npos) p = p.substr(k + 1);
            }
            r = p;
        } else if (f == "FILESIZE") {
            if (u_.filename.empty() || !util::file_exists(u_.filename)) return std::nullopt;
            auto sz = util::file_size(u_.filename);
            if (in.has("\\k")) r = std::to_string((sz + 1023) / 1024);
            else if (in.has("\\m")) r = std::to_string((sz + 1048575) / 1048576);
            else r = std::to_string(sz);
        } else if (f == "TEMPLATE") {
            auto app = app_properties(d_);
            r = app.contains("Template") ? app["Template"].get<std::string>() : "Normal.dotm";
        } else if (f == "NUMWORDS" || f == "NUMCHARS") {
            auto st = statistics(d_);
            r = std::to_string(st[f == "NUMWORDS" ? "words" : "characters_no_spaces"].get<long>());
        } else if (f == "USERNAME") {
            r = d_.author.name;
        } else if (f == "USERINITIALS") {
            std::string ini;
            bool start = true;
            for (char c : d_.author.name) {
                if (std::isspace((unsigned char)c)) { start = true; continue; }
                if (start) ini += (char)std::toupper((unsigned char)c);
                start = false;
            }
            r = d_.author.initials.empty() ? ini : d_.author.initials;
        } else if (f == "QUOTE") {
            std::string s;
            for (auto& a : in.args) s += (s.empty() ? "" : " ") + a.text;
            r = s;
        } else if (f == "SET") {
            if (in.args.size() >= 1) set_[util::lower(in.arg(0))] = in.args.size() > 1 ? in.arg(1) : "";
            r = "";
        } else if (f == "IF") {
            std::size_t used = 0;
            auto c = condition(in.args, used);
            if (!c) return std::nullopt;
            std::string yes = used < in.args.size() ? in.args[used].text : "";
            std::string no = used + 1 < in.args.size() ? in.args[used + 1].text : "";
            r = *c ? yes : no;
        } else if (f == "COMPARE") {
            std::size_t used = 0;
            auto c = condition(in.args, used);
            if (!c) return std::string("!Syntax Error");
            r = *c ? "1" : "0";
        } else if (f == "=") {
            std::string expr;
            for (auto& a : in.args) expr += (expr.empty() ? "" : " ") + a.text;
            Expr e;
            e.s = expr;
            e.name = [&](const std::string& n) -> std::optional<double> {
                if (auto v = cell_ref(fi, n)) return v;
                return bookmark_number(n);
            };
            e.range = [&](const std::string& dir) { return cell_range(fi, dir); };
            double v = e.cmp();
            e.ws();
            if (e.bad || e.i < e.s.size()) return std::string("!Syntax Error");
            r = in.has("\\#") ? format_numeric(v, in.get("\\#")) : number_text(v);
            if (in.has("\\#")) return apply_format(*r, in);
        } else if (f == "MERGEFIELD") {
            auto v = merge_value(in.arg(0));
            if (!v) return std::nullopt;
            r = *v;
            if (!r->empty()) {
                if (in.has("\\b")) r = in.get("\\b") + *r;
                if (in.has("\\f")) *r += in.get("\\f");
            }
        } else if (f == "MERGEREC" || f == "MERGESEQ") {
            if (!u_.merge) return std::nullopt;
            r = std::to_string(u_.merge->row + 1);
        } else if (f == "NEXT") {
            if (!u_.merge) return std::nullopt;
            u_.merge->row++;
            r = "";
        } else if (f == "NEXTIF" || f == "SKIPIF") {
            if (!u_.merge) return std::nullopt;
            std::size_t used = 0;
            auto c = condition(in.args, used);
            if (c && *c) {
                if (f == "NEXTIF") u_.merge->row++;
                else u_.merge->skip = true;
            }
            r = "";
        } else if (f == "LISTNUM") {
            return std::nullopt;
        } else if (f == "HYPERLINK" || f == "TOC" || f == "INDEX" || f == "XE" || f == "TC" || f == "RD" || f == "TA" ||
                   f == "TOA" || f == "PRIVATE" || f == "ADDIN" || f == "EMBED" || f == "INCLUDEPICTURE" || f == "INCLUDETEXT" ||
                   f == "FORMTEXT" || f == "FORMCHECKBOX" || f == "FORMDROPDOWN" || f == "MACROBUTTON" || f == "GOTOBUTTON" ||
                   f == "BIBLIOGRAPHY" || f == "CITATION" || f == "EQ" || f == "SYMBOL" || f == "AUTOTEXT" || f == "ASK" ||
                   f == "FILLIN" || f == "LINK" || f == "DDE" || f == "DDEAUTO" || f == "BARCODE" || f == "DISPLAYBARCODE") {
            return std::nullopt;  // results kept: Word's own or not text we compute
        } else {
            return std::nullopt;
        }
        if (!r) return std::nullopt;
        if (in.has("\\#") && f != "=") {
            double v = strict_number(*r);
            if (!std::isnan(v)) r = format_numeric(v, in.get("\\#"));
        }
        if (in.has("\\@") && f != "DATE" && f != "TIME" && f != "CREATEDATE" && f != "SAVEDATE" && f != "PRINTDATE") {
            // a date-valued result (MERGEFIELD Date \@ "MMMM d, yyyy")
            std::string s = util::trim(*r);
            std::tm t{};
            bool ok = false;
            if (s.size() >= 10 && s[4] == '-' && s[7] == '-') { t = date_of(s); ok = true; }
            else {
                int m = 0, dd = 0, y = 0;
                if (std::sscanf(s.c_str(), "%d/%d/%d", &m, &dd, &y) == 3) {
                    t.tm_year = (y < 100 ? y + 2000 : y) - 1900;
                    t.tm_mon = m - 1;
                    t.tm_mday = dd;
                    std::tm c = t;
                    std::mktime(&c);
                    t.tm_wday = c.tm_wday;
                    ok = true;
                }
            }
            if (ok) r = format_date(t, in.get("\\@"));
        }
        return apply_format(*r, in);
    }

private:
    // Evaluate and write a top-level field's result. Nested fields inside
    // the result (IF yes/no texts holding MERGEFIELDs) are part of the code.
    bool update(int fi) {
        auto code = util::trim(code_of(fi));
        Instr in = parse(code);
        // nested fields in the result part (e.g. an IF's result holding fields) get updated too
        auto& f = fields_[fi];
        if (f.begin && f.sep) {
            for (int k : f.kids) {
                auto& kf = fields_[k];
                if (kf.begin && before(f.sep, kf.begin)) update(k);
                else if (kf.simple && before(f.sep, kf.simple)) update(k);
            }
        }
        auto v = eval(fi, code);
        if (!v) return false;
        std::string res = *v;
        if (in.name == "PAGEREF" || in.name == "REF") {
            // keep line breaks out of a REF to a whole paragraph
            res = trim_para(res);
        }
        if (res == current_result(fi)) return false;
        write_result(fi, res);
        return true;
    }

public:
    void write_result(int fi, const std::string& res) {
        bms_loaded_ = false;  // bookmark text may sit in this result
        auto& f = fields_[fi];
        if (f.simple) {
            pugi::xml_node rpr;
            if (auto r0 = f.simple.child("w:r")) rpr = r0.child("w:rPr");
            pugi::xml_document keep;
            if (rpr) keep.append_copy(rpr);
            std::vector<pugi::xml_node> old;
            for (auto c : f.simple.children()) old.push_back(c);
            for (auto c : old) f.simple.remove_child(c);
            auto nr = f.simple.append_child("w:r");
            if (keep.first_child()) nr.append_copy(keep.first_child());
            append_text(nr, util::to_u32(res));
            return;
        }
        if (!f.sep) {
            // add a separate mark before the end
            auto er = f.end.parent();
            auto sr = er.parent().insert_child_before("w:r", er);
            if (auto rp = er.child("w:rPr")) sr.append_copy(rp);
            xml::set_attr(sr.append_child("w:fldChar"), "w:fldCharType", "separate");
            f.sep = sr.child("w:fldChar");
        }
        auto sep_run = f.sep.parent();
        auto end_run = f.end.parent();
        // collect result runs between separate and end (same paragraph only:
        // multi-paragraph results such as TOC/INDEX aren't written here)
        if (sep_run.parent() != end_run.parent()) {
            // keep the first result paragraph's runs, drop the rest of the text
            return;
        }
        // replace the text of the result runs, keeping bookmarks and other
        // markers where they are; the first text run keeps its formatting
        pugi::xml_node first;
        std::vector<pugi::xml_node> runs;
        for (auto n = sep_run.next_sibling(); n && n != end_run; n = n.next_sibling()) {
            if (xml::name_is(n, "w:r")) runs.push_back(n);
            else if (is_revision_container(n) || xml::name_is(n, "w:smartTag") || xml::name_is(n, "w:customXml"))
                for (auto r : xml::descendants(n, "w:r")) runs.push_back(r);
        }
        auto parent = end_run.parent();
        for (auto r : runs) {
            if (xml::ancestor(r, "w:del")) continue;
            bool text = false;
            std::vector<pugi::xml_node> drop;
            for (auto c : r.children()) {
                if (c.type() != pugi::node_element || xml::name_is(c, "w:rPr")) continue;
                drop.push_back(c);
                text = true;
            }
            for (auto c : drop) r.remove_child(c);
            if (!first && text) { first = r; continue; }
            if (r.parent()) {
                auto pr = r.parent();
                pr.remove_child(r);
                if (is_revision_container(pr) && !pr.first_child()) pr.parent().remove_child(pr);
            }
        }
        if (res.empty()) {
            if (first) first.parent().remove_child(first);
            return;
        }
        if (!first) {
            first = parent.insert_child_before("w:r", end_run);
            if (auto rp = sep_run.child("w:rPr")) first.append_copy(rp);
        }
        append_text(first, util::to_u32(res));
    }
};

}  // namespace

int evaluate_fields(Document& d, const FieldUpdate& u, const PageMap* pages) {
    Engine e(d, u, pages);
    return e.run();
}

FieldUpdateResult update_fields(Document& d, const FieldUpdate& u) {
    FieldUpdateResult r;
    std::optional<PageMap> pm;
    if (u.layout) {
        // page numbers depend on the TOC's length: rebuild first, lay out, then fill in
        r.updated += detail::rebuild_tocs(d);
        pm = page_map(d);
        r.pages = true;
    } else {
        r.updated += detail::rebuild_tocs(d);
    }
    r.updated += evaluate_fields(d, u, pm ? &*pm : nullptr);
    r.updated += rebuild_bibliographies(d, "");
    if (pm) {
        // TOC page numbers: each entry's PAGEREF hidden bookmark now has a page
        // (handled by evaluate_fields through PAGEREF). Index entries next.
        std::map<std::string, std::vector<std::string>> pages;
        auto anchors = field_anchors(d.body());
        for (std::size_t i = 0; i < anchors.size() && i < pm->fields.size(); i++) {
            auto term = detail::xe_term(detail::field_code(anchors[i]));
            if (term.empty()) continue;
            std::string main = term, sub;
            if (auto c = term.find(':'); c != std::string::npos) { main = term.substr(0, c); sub = term.substr(c + 1); }
            std::replace(main.begin(), main.end(), '\x01', ':');
            std::replace(sub.begin(), sub.end(), '\x01', ':');
            std::string key = util::lower(main) + (sub.empty() ? "" : "\x02" + util::lower(sub));
            std::string pg = std::to_string(pm->fields[i]);
            auto& v = pages[key];
            if (std::find(v.begin(), v.end(), pg) == v.end()) v.push_back(pg);
        }
        r.updated += detail::rebuild_indexes(d, &pages);
        // a second pass: the TOC/index text may have moved pages
        auto pm2 = page_map(d);
        evaluate_fields(d, u, &pm2);
    } else {
        detail::set_update_fields(d);  // Word fills page numbers on open
    }
    if (pm) {
        // results are current: Word needn't prompt to update on open
        if (auto st = d.settings(false))
            if (auto uf = st.child("w:updateFields")) st.remove_child(uf);
    }
    return r;
}

// ── shared helpers ───────────────────────────────────────────────────────

std::string detail::field_code(pugi::xml_node a) {
    if (xml::name_is(a, "w:fldSimple")) return util::trim(xml::attr(a, "w:instr"));
    std::string s;
    auto root = xml::root_of(a);
    int depth = 0;
    for (auto n = next_in(a, root); n; n = next_in(n, root)) {
        if (xml::name_is(n, "w:fldChar")) {
            std::string t = xml::attr(n, "w:fldCharType");
            if (t == "begin") depth++;
            else if (t == "end") { if (!depth) break; depth--; }
            else if (t == "separate" && !depth) break;
        }
        if (!depth && xml::name_is(n, "w:instrText")) s += n.text().get();
    }
    return util::trim(s);
}

pugi::xml_node detail::field_end(pugi::xml_node b) {
    auto root = xml::root_of(b);
    int depth = 0;
    for (auto n = next_in(b, root); n; n = next_in(n, root)) {
        if (!xml::name_is(n, "w:fldChar")) continue;
        std::string t = xml::attr(n, "w:fldCharType");
        if (t == "begin") depth++;
        else if (t == "end") { if (!depth) return n; depth--; }
    }
    return {};
}

pugi::xml_node detail::next_node(pugi::xml_node n, pugi::xml_node root) { return next_in(n, root); }

pugi::xml_node detail::after_subtree(pugi::xml_node n, pugi::xml_node root) {
    while (n && n != root) {
        if (n.next_sibling()) return n.next_sibling();
        n = n.parent();
    }
    return {};
}

// ── unlink (Ctrl+Shift+F9) ───────────────────────────────────────────────

int unlink_fields(Document& d, const std::vector<std::string>& kinds_in) {
    std::set<std::string> kinds;
    for (auto& k : kinds_in) kinds.insert(util::upper(util::trim(k)));
    int n = 0;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        // innermost first so nested fields' results stay in place
        for (bool again = true; again;) {
            again = false;
            auto fs = scan(root, part);
            for (int i = (int)fs.size() - 1; i >= 0; i--) {
                auto& f = fs[i];
                if (!f.kids.empty()) {
                    // only unlink a parent after its kids are gone
                    bool live = false;
                    for (int k : f.kids) if (fs[k].begin || fs[k].simple) live = true;
                    if (live) continue;
                }
                std::string code = f.simple ? xml::attr(f.simple, "w:instr") : detail::field_code(f.begin);
                std::string name = util::upper(parse(code).name);
                if (!kinds.empty() && !kinds.count(name)) continue;
                if (name == "TOC" || name == "INDEX" || name == "XE" || name == "TC") {
                    if (kinds.empty()) {} else {}
                }
                if (f.simple) {
                    auto parent = f.simple.parent();
                    std::vector<pugi::xml_node> kids;
                    for (auto c : f.simple.children()) kids.push_back(c);
                    for (auto c : kids) parent.insert_move_before(c, f.simple);
                    parent.remove_child(f.simple);
                    n++;
                    again = true;
                    break;
                }
                if (!f.begin || !f.end) continue;
                // remove runs from begin to sep (code) and the sep/end runs; keep result runs.
                // XE / TC / RD have no result: remove the whole thing.
                bool no_result = name == "XE" || name == "TC" || name == "RD" || name == "SET" || name == "NEXT" ||
                                 name == "NEXTIF" || name == "SKIPIF";
                auto stop = f.sep && !no_result ? f.sep : f.end;
                std::vector<pugi::xml_node> kill;
                // collect top-level run containers between begin and stop (inclusive),
                // plus the end run. Runs can span paragraphs; gather by walking.
                auto b = f.begin.parent();
                for (auto x = b; x;) {
                    if (xml::name_is(x, "w:r")) {
                        bool is_stop = x == stop.parent();
                        kill.push_back(x);
                        if (is_stop) break;
                        x = detail::after_subtree(x, root);
                        continue;
                    }
                    if (xml::name_is(x, "w:p") || x.first_child()) {
                        // descend into containers (hyperlink, ins, smartTag...) and next paragraphs
                        bool has_stop = false;
                        for (auto y = x.first_child(); y; y = detail::next_node(y, x)) if (y == stop) { has_stop = true; break; }
                        if (has_stop || xml::name_is(x, "w:p")) { x = x.first_child(); continue; }
                    }
                    x = detail::after_subtree(x, root);
                }
                if (!no_result) kill.push_back(f.end.parent());
                for (auto r : kill) {
                    // a run holding only field chars / instr text goes; else just drop those children
                    std::vector<pugi::xml_node> fc;
                    for (auto c : r.children())
                        if (xml::name_is(c, "w:fldChar") || xml::name_is(c, "w:instrText") || xml::name_is(c, "w:delInstrText")) fc.push_back(c);
                    for (auto c : fc) r.remove_child(c);
                    bool empty = true;
                    for (auto c : r.children()) if (c.type() == pugi::node_element && !xml::name_is(c, "w:rPr")) empty = false;
                    if (empty && r.parent()) r.parent().remove_child(r);
                }
                n++;
                again = true;
                break;
            }
        }
    }
    return n;
}

// ── document variables ───────────────────────────────────────────────────

Json document_variables(Document& d) {
    Json j = Json::object();
    auto st = d.settings(false);
    if (!st) return j;
    for (auto v : xml::children(st.child("w:docVars"), "w:docVar")) j[xml::attr(v, "w:name")] = xml::attr(v, "w:val");
    return j;
}

void set_document_variable(Document& d, const std::string& name, const Json& value) {
    if (name.empty()) throw std::runtime_error("a document variable needs a name");
    auto st = d.settings();
    auto vars = st.child("w:docVars");
    for (auto v : xml::children(vars, "w:docVar"))
        if (xml::attr(v, "w:name") == name) {
            if (value.is_null()) vars.remove_child(v);
            else xml::set_attr(v, "w:val", value.is_string() ? value.get<std::string>() : value.dump());
            if (!vars.child("w:docVar")) st.remove_child(vars);
            return;
        }
    if (value.is_null()) return;
    vars = schema::ensure_child(st, "w:docVars");
    auto v = vars.append_child("w:docVar");
    xml::set_attr(v, "w:name", name);
    xml::set_attr(v, "w:val", util::xml_safe(value.is_string() ? value.get<std::string>() : value.dump()));
}

// ── mail merge ───────────────────────────────────────────────────────────

std::vector<std::string> merge_field_names(Document& d) {
    std::vector<std::string> v;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto& f : scan(root, part)) {
            std::string code = f.simple ? xml::attr(f.simple, "w:instr") : (f.begin ? detail::field_code(f.begin) : "");
            auto in = parse(code);
            if (in.name == "MERGEFIELD" && !in.arg(0).empty() && std::find(v.begin(), v.end(), in.arg(0)) == v.end())
                v.push_back(in.arg(0));
        }
    }
    return v;
}

namespace {

std::vector<std::vector<std::string>> parse_delimited(const std::string& s, char sep) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool q = false, any = false;
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') { cell += '"'; i++; }
                else q = false;
            } else cell += c;
            continue;
        }
        if (c == '"' && cell.empty()) { q = true; any = true; continue; }
        if (c == sep) { row.push_back(cell); cell.clear(); any = true; continue; }
        if (c == '\r') continue;
        if (c == '\n') {
            row.push_back(cell);
            cell.clear();
            if (any || row.size() > 1 || !row[0].empty()) rows.push_back(row);
            row.clear();
            any = false;
            continue;
        }
        cell += c;
        any = true;
    }
    if (any || !cell.empty()) { row.push_back(cell); rows.push_back(row); }
    return rows;
}

}  // namespace

Json load_merge_data(const std::string& path) {
    auto bytes = util::read_file(path);
    std::string s(bytes.begin(), bytes.end());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s = s.substr(3);
    std::string ext = util::lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
    if (ext == ".json") {
        Json j = Json::parse(s, nullptr, false);
        if (j.is_discarded()) throw std::runtime_error("the data file isn't valid JSON");
        if (j.is_object() && j.contains("rows")) j = j["rows"];
        if (!j.is_array()) throw std::runtime_error("the JSON data must be an array of objects (one per record)");
        return j;
    }
    char sep = ext == ".tsv" || ext == ".tab" ? '\t' : ',';
    if (ext != ".csv" && ext != ".tsv" && ext != ".tab" && ext != ".txt")
        throw std::runtime_error("data files are .csv, .tsv or .json");
    if (ext == ".txt") {
        auto nl = s.find('\n');
        std::string head = s.substr(0, nl);
        sep = std::count(head.begin(), head.end(), '\t') > std::count(head.begin(), head.end(), ',') ? '\t' : ',';
    }
    auto rows = parse_delimited(s, sep);
    if (rows.empty()) throw std::runtime_error("the data file is empty");
    Json out = Json::array();
    auto& head = rows[0];
    for (std::size_t i = 1; i < rows.size(); i++) {
        Json r = Json::object();
        for (std::size_t k = 0; k < head.size(); k++) r[util::trim(head[k])] = k < rows[i].size() ? rows[i][k] : "";
        out.push_back(r);
    }
    return out;
}

std::vector<Document> mail_merge(Document& tmpl, const Json& rows, bool single, const std::string& filename,
                                 std::vector<std::string>* notes, std::vector<std::size_t>* sources) {
    if (!rows.is_array()) throw std::runtime_error("records must be an array of objects");
    if (rows.empty()) throw std::runtime_error("there are no records to merge");
    for (auto& r : rows)
        if (!r.is_object()) throw std::runtime_error("each record must be an object of field name -> value");
    auto base = tmpl.snapshot();
    MergeCursor cur;
    cur.rows = &rows;
    std::vector<Document> out;
    std::vector<util::Bytes> merged;
    int skipped = 0;
    while (cur.row < rows.size()) {
        Document d = Document::from_bytes(base);
        d.author = tmpl.author;
        cur.skip = false;
        std::size_t first_row = cur.row;
        FieldUpdate u;
        u.merge = &cur;
        u.filename = filename;
        evaluate_fields(d, u, nullptr);
        cur.row++;
        if (cur.skip) { skipped++; continue; }
        if (sources) sources->push_back(first_row);
        // the merged document holds text, not merge fields
        unlink_fields(d, {"MERGEFIELD", "MERGEREC", "MERGESEQ", "NEXT", "NEXTIF", "SKIPIF", "IF", "SET", "ASK", "FILLIN"});
        // the mail merge data source settings belong to the main document only
        if (auto st = d.settings(false))
            if (auto mm = st.child("w:mailMerge")) st.remove_child(mm);
        if (single) merged.push_back(d.snapshot());
        else out.push_back(std::move(d));
    }
    if (notes) {
        if (skipped) notes->push_back(std::to_string(skipped) + " record(s) skipped by SKIPIF");
        for (auto& m : cur.missing) notes->push_back("no column named '" + m + "' in the data: left blank");
    }
    if (!single) return out;
    if (merged.empty()) throw std::runtime_error("every record was skipped");
    // one document: each record's body after the previous, separated by a next-page section break
    Document all = Document::from_bytes(merged[0]);
    all.author = tmpl.author;
    auto body = all.body();
    auto final_sect = body.child("w:sectPr");
    for (std::size_t i = 1; i < merged.size(); i++) {
        Document rec = Document::from_bytes(merged[i]);
        // close the previous record with a section break carrying its section properties
        auto blocks_now = blocks(body);
        pugi::xml_node last_p;
        for (auto it = blocks_now.rbegin(); it != blocks_now.rend(); ++it)
            if (xml::name_is(*it, "w:p") && it->parent() == body) { last_p = *it; break; }
        if (!last_p || last_p.child("w:pPr").child("w:sectPr")) {
            last_p = body.insert_child_before("w:p", final_sect);
        }
        auto ppr = schema::ensure_child(last_p, "w:pPr");
        auto sp = ppr.append_copy(final_sect);
        schema::place_child(ppr, sp);
        if (!sp.child("w:type")) { auto t = schema::ensure_child(sp, "w:type"); xml::set_attr(t, "w:val", "nextPage"); }
        for (auto c : rec.body().children()) {
            if (xml::name_is(c, "w:sectPr")) continue;
            body.insert_copy_before(c, final_sect);
        }
    }
    // pasted copies repeat paragraph ids and bookmark/annotation ids: make them unique
    for (auto p : xml::descendants(body, "w:p")) {
        p.remove_attribute("w14:paraId");
        p.remove_attribute("w14:textId");
    }
    all.ensure_para_ids(body);
    all.dedupe_ids();
    std::vector<Document> one;
    one.push_back(std::move(all));
    return one;
}

}  // namespace docx::wml

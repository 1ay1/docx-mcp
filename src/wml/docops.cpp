#include "wml/docops.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/comments.hpp"
#include "wml/edit.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

std::string format_number(int n, const std::string& fmt);  // fields.cpp

namespace {

// Word 2007 "legacy" password key (MS-OFFCRYPTO 2.3.7.1/2.3.7.4), the
// pre-processing step before the SHA hash for document protection.
const std::uint16_t kInitialCode[15] = {0xE1F0, 0x1D0F, 0xCC9C, 0x84C0, 0x110C, 0x0E10, 0xF1CE, 0x313E,
                                        0x1872, 0xE139, 0xD40F, 0x84F9, 0x280C, 0xA96A, 0x4EC3};
const std::uint16_t kMatrix[15][7] = {
    {0xAEFC, 0x4DD9, 0x9BB2, 0x2745, 0x4E8A, 0x9D14, 0x2A09}, {0x7B61, 0xF6C2, 0xFDA5, 0xEB6B, 0xC6F7, 0x9DCF, 0x2BBF},
    {0x4563, 0x8AC6, 0x05AD, 0x0B5A, 0x16B4, 0x2D68, 0x5AD0}, {0x0375, 0x06EA, 0x0DD4, 0x1BA8, 0x3750, 0x6EA0, 0xDD40},
    {0xD849, 0xA0B3, 0x5147, 0xA28E, 0x553D, 0xAA7A, 0x44D5}, {0x6F45, 0xDE8A, 0xAD35, 0x4A4B, 0x9496, 0x390D, 0x721A},
    {0xEB23, 0xC667, 0x9CEF, 0x29FF, 0x53FE, 0xA7FC, 0x5FD9}, {0x47D3, 0x8FA6, 0x0F6D, 0x1EDA, 0x3DB4, 0x7B68, 0xF6D0},
    {0xB861, 0x60E3, 0xC1C6, 0x93AD, 0x377B, 0x6EF6, 0xDDEC}, {0x45A0, 0x8B40, 0x06A1, 0x0D42, 0x1A84, 0x3508, 0x6A10},
    {0xAA51, 0x4483, 0x8906, 0x022D, 0x045A, 0x08B4, 0x1168}, {0x76B4, 0xED68, 0xCAF1, 0x85C3, 0x1BA7, 0x374E, 0x6E9C},
    {0x3730, 0x6E60, 0xDCC0, 0xA9A1, 0x4363, 0x86C6, 0x1DAD}, {0x3331, 0x6662, 0xCCC4, 0x89A9, 0x0373, 0x06E6, 0x0DCC},
    {0x1021, 0x2042, 0x4084, 0x8108, 0x1231, 0x2462, 0x48C4}};

std::string legacy_key_hex(const std::string& password) {
    // ANSI bytes: low byte of each UTF-16 unit, or the high byte when low is 0; max 15
    std::u16string u = util::to_u16(password);
    std::vector<std::uint8_t> chars;
    for (char16_t c : u) {
        std::uint8_t lo = c & 0xFF, hi = c >> 8;
        chars.push_back(lo ? lo : hi);
        if (chars.size() == 15) break;
    }
    std::uint32_t key = 0;
    if (!chars.empty()) {
        std::uint16_t high = kInitialCode[chars.size() - 1];
        int line = 15 - (int)chars.size();
        for (auto ch : chars) {
            for (int bit = 0; bit < 7; bit++) {
                if (ch & 1) high ^= kMatrix[line][bit];
                ch >>= 1;
            }
            line++;
        }
        // CreatePasswordVerifier_Method1: from the last char to the first,
        // verifier = rotl15(verifier) ^ char; then rotl15 ^ length ^ 0xCE4B
        std::uint16_t low = 0;
        auto rot = [](std::uint16_t v) { return std::uint16_t(((v >> 14) & 1) | ((v << 1) & 0x7FFF)); };
        for (auto it = chars.rbegin(); it != chars.rend(); ++it) low = std::uint16_t(rot(low) ^ *it);
        low = std::uint16_t(rot(low) ^ std::uint16_t(chars.size()) ^ 0xCE4B);
        key = (std::uint32_t(high) << 16) | low;
    }
    // byte order reversed, as uppercase hex (MS-OI29500: "0x64CEED7E becomes 7EEDCE64")
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X%02X", key & 0xFF, (key >> 8) & 0xFF, (key >> 16) & 0xFF, (key >> 24) & 0xFF);
    return buf;
}

util::HashAlg alg_of(const std::string& name) {
    std::string n = util::lower(name);
    if (n == "sha-1" || n == "sha1") return util::HashAlg::SHA1;
    if (n == "sha-256" || n == "sha256") return util::HashAlg::SHA256;
    if (n == "sha-384" || n == "sha384") return util::HashAlg::SHA384;
    if (n == "sha-512" || n == "sha512") return util::HashAlg::SHA512;
    throw std::runtime_error("unsupported protection hash algorithm " + name);
}

util::HashAlg alg_of_sid(int sid) {
    switch (sid) {
        case 4: return util::HashAlg::SHA1;
        case 12: return util::HashAlg::SHA256;
        case 13: return util::HashAlg::SHA384;
        case 14: return util::HashAlg::SHA512;
    }
    throw std::runtime_error("unsupported legacy protection hash id " + std::to_string(sid));
}

}  // namespace

std::string protection_hash(const std::string& password, const util::Bytes& salt, int spin, util::HashAlg alg) {
    // Word: legacy key hex -> UTF-16LE, prefixed with the binary salt, hashed;
    // then spin iterations of H(hash || iterator as 4-byte LE). The initial
    // hash is not counted (MS-OI29500 2.1.402 b, e).
    std::string keyhex = legacy_key_hex(password);
    util::Bytes buf(salt);
    for (char c : keyhex) { buf.push_back(std::uint8_t(c)); buf.push_back(0); }
    util::Bytes h = util::hash(alg, buf);
    for (int i = 0; i < spin; i++) {
        util::Bytes b2(h);
        for (int k = 0; k < 4; k++) b2.push_back(std::uint8_t((std::uint32_t(i) >> (8 * k)) & 0xFF));
        h = util::hash(alg, b2);
    }
    return util::base64_encode(h);
}

void protect(Document& d, const std::string& mode, const std::string& password) {
    static const std::set<std::string> modes = {"readOnly", "comments", "trackedChanges", "forms", "none"};
    if (!modes.count(mode)) throw std::runtime_error("mode must be readOnly, comments, trackedChanges, forms or none");
    auto st = d.settings();
    st.remove_child("w:documentProtection");
    auto dp = schema::ensure_child(st, "w:documentProtection");
    xml::set_attr(dp, "w:edit", mode);
    xml::set_attr(dp, "w:enforcement", mode == "none" ? "0" : "1");
    if (!password.empty()) {
        // Word 2010+ writes the ISO/ECMA attribute set with SHA-512, 100000 spins
        util::Bytes salt = util::random_bytes(16);
        int spin = 100000;
        xml::set_attr(dp, "w:cryptProviderType", "rsaAES");
        xml::set_attr(dp, "w:cryptAlgorithmClass", "hash");
        xml::set_attr(dp, "w:cryptAlgorithmType", "typeAny");
        xml::set_attr(dp, "w:cryptAlgorithmSid", "14");
        xml::set_attr(dp, "w:cryptSpinCount", std::to_string(spin));
        xml::set_attr(dp, "w:hash", protection_hash(password, salt, spin, util::HashAlg::SHA512));
        xml::set_attr(dp, "w:salt", util::base64_encode(salt));
    }
    // tracked-changes protection also turns tracking on
    if (mode == "trackedChanges") d.set_track_changes(true);
    // attribute order is fixed by the schema's attribute list; nothing to sort
}

Json protection_info(Document& d) {
    Json j;
    auto st = d.settings(false);
    auto dp = st ? st.child("w:documentProtection") : pugi::xml_node{};
    if (!dp) { j["protected"] = false; return j; }
    std::string enf = xml::attr(dp, "w:enforcement", "1");  // Word enforces when missing (MS-OI29500 c)
    j["protected"] = enf == "1" || enf == "true" || enf == "on";
    j["mode"] = xml::attr(dp, "w:edit", "none");
    j["has_password"] = !xml::attr(dp, "w:hash").empty() || !xml::attr(dp, "w:hashValue").empty();
    j["formatting_restricted"] = xml::attr(dp, "w:formatting") == "1";
    return j;
}

bool check_password(Document& d, const std::string& password) {
    auto st = d.settings(false);
    auto dp = st ? st.child("w:documentProtection") : pugi::xml_node{};
    if (!dp) return true;
    if (!xml::attr(dp, "w:hashValue").empty()) {
        // ECMA attribute set: algorithmName/hashValue/saltValue/spinCount
        auto salt = util::base64_decode(xml::attr(dp, "w:saltValue"));
        if (!salt) return false;
        int spin = std::stoi(xml::attr(dp, "w:spinCount", "0"));
        return protection_hash(password, *salt, spin, alg_of(xml::attr(dp, "w:algorithmName", "SHA-1"))) == xml::attr(dp, "w:hashValue");
    }
    if (!xml::attr(dp, "w:hash").empty()) {
        auto salt = util::base64_decode(xml::attr(dp, "w:salt"));
        if (!salt) return false;
        int spin = std::stoi(xml::attr(dp, "w:cryptSpinCount", "0"));
        int sid = std::stoi(xml::attr(dp, "w:cryptAlgorithmSid", "4"));
        return protection_hash(password, *salt, spin, alg_of_sid(sid)) == xml::attr(dp, "w:hash");
    }
    return true;  // no password set
}

void unprotect(Document& d, const std::string& password) {
    if (!check_password(d, password)) throw std::runtime_error("wrong password");
    if (auto st = d.settings(false)) st.remove_child("w:documentProtection");
}

void set_read_only_recommended(Document& d, bool on) {
    auto st = d.settings();
    if (on) xml::set_attr(schema::ensure_child(st, "w:writeProtection"), "w:recommended", "1");
    else st.remove_child("w:writeProtection");
}

// ── properties ───────────────────────────────────────────────────────────

namespace {
const std::map<std::string, std::string>& core_map() {
    static const std::map<std::string, std::string> m = {
        {"title", "dc:title"}, {"subject", "dc:subject"}, {"creator", "dc:creator"}, {"author", "dc:creator"},
        {"keywords", "cp:keywords"}, {"description", "dc:description"}, {"comments", "dc:description"},
        {"category", "cp:category"}, {"last_modified_by", "cp:lastModifiedBy"}, {"revision", "cp:revision"},
        {"content_status", "cp:contentStatus"}, {"status", "cp:contentStatus"}, {"language", "dc:language"},
        {"identifier", "dc:identifier"}, {"version", "cp:version"}, {"created", "dcterms:created"},
        {"modified", "dcterms:modified"}, {"last_printed", "cp:lastPrinted"}};
    return m;
}
}  // namespace

Json core_properties(Document& d) {
    Json j = Json::object();
    auto cp = d.core_props(false);
    if (!cp) return j;
    for (auto c : cp.children()) {
        if (c.type() != pugi::node_element) continue;
        std::string n = c.name();
        for (auto& [k, q] : core_map()) {
            if (q == n && !j.contains(k)) {
                j[k] = c.text().get();
                break;
            }
        }
    }
    j.erase("author");
    j.erase("comments");
    j.erase("status");
    return j;
}

void set_core_properties(Document& d, const Json& props) {
    auto cp = d.core_props(true);
    // canonical order Word writes
    static const char* order[] = {"dc:title", "dc:subject", "dc:creator", "cp:keywords", "dc:description", "cp:lastModifiedBy",
                                  "cp:revision", "cp:lastPrinted", "dcterms:created", "dcterms:modified", "cp:category",
                                  "cp:contentStatus", "dc:language", "dc:identifier", "cp:version"};
    for (auto& [k, v] : props.items()) {
        auto it = core_map().find(k);
        if (it == core_map().end()) throw std::runtime_error("unknown document property '" + k + "'");
        std::string q = it->second;
        auto e = cp.child(q.c_str());
        if (v.is_null()) { if (e) cp.remove_child(e); continue; }
        if (!e) {
            int my = 0;
            for (int i = 0; i < 15; i++) if (q == order[i]) my = i;
            pugi::xml_node after;
            for (auto c : cp.children()) {
                for (int i = 0; i < my; i++) if (std::string_view(c.name()) == order[i]) after = c;
            }
            e = after ? cp.insert_child_after(q.c_str(), after) : cp.prepend_child(q.c_str());
            if (util::starts_with(q, "dcterms:")) {
                xml::set_attr(e, "xsi:type", "dcterms:W3CDTF");
                if (!cp.attribute("xmlns:xsi")) xml::set_attr(cp, "xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
                if (!cp.attribute("xmlns:dcterms")) xml::set_attr(cp, "xmlns:dcterms", "http://purl.org/dc/terms/");
            }
        }
        std::string s = v.is_string() ? v.get<std::string>() : v.dump();
        e.text().set(util::xml_safe(s).c_str());
    }
}

Json custom_properties(Document& d) {
    Json j = Json::object();
    auto cp = d.custom_props(false);
    if (!cp) return j;
    for (auto p : cp.children()) {
        if (p.type() != pugi::node_element) continue;
        std::string n = xml::attr(p, "name");
        auto v = p.first_child();
        while (v && v.type() != pugi::node_element) v = v.next_sibling();
        if (!v) continue;
        std::string t = v.name();
        std::string text = v.text().get();
        if (t == "vt:bool") j[n] = text == "true" || text == "1";
        else if (t == "vt:i4" || t == "vt:i8" || t == "vt:int") j[n] = std::stoll(text);
        else if (t == "vt:r8" || t == "vt:r4") j[n] = std::stod(text);
        else j[n] = text;
    }
    return j;
}

void set_custom_property(Document& d, const std::string& name, const Json& value) {
    auto cp = d.custom_props(true);
    pugi::xml_node existing;
    int maxpid = 1;
    for (auto p : cp.children()) {
        if (p.type() != pugi::node_element) continue;
        maxpid = std::max(maxpid, p.attribute("pid").as_int());
        if (xml::attr(p, "name") == name) existing = p;
    }
    if (value.is_null()) { if (existing) cp.remove_child(existing); return; }
    auto p = existing ? existing : cp.append_child("op:property");
    if (!existing) {
        p.append_attribute("fmtid") = "{D5CDD505-2E9C-101B-9397-08002B2CF9AE}";
        p.append_attribute("pid") = maxpid + 1;
        p.append_attribute("name") = name.c_str();
    }
    while (p.first_child()) p.remove_child(p.first_child());
    if (value.is_boolean()) p.append_child("vt:bool").text().set(value.get<bool>() ? "true" : "false");
    else if (value.is_number_integer()) p.append_child("vt:i4").text().set(std::to_string(value.get<long long>()).c_str());
    else if (value.is_number()) p.append_child("vt:r8").text().set(std::to_string(value.get<double>()).c_str());
    else p.append_child("vt:lpwstr").text().set(util::xml_safe(value.is_string() ? value.get<std::string>() : value.dump()).c_str());
}

Json app_properties(Document& d) {
    Json j = Json::object();
    auto ap = d.app_props(false);
    if (!ap) return j;
    for (auto c : ap.children())
        if (c.type() == pugi::node_element && !c.first_child().first_child()) {
            std::string n = c.name();
            if (auto k = n.find(':'); k != std::string::npos) n = n.substr(k + 1);
            j[n] = c.text().get();
        }
    return j;
}

// ── settings ─────────────────────────────────────────────────────────────

Json settings_json(Document& d) {
    Json j;
    auto st = d.settings(false);
    if (!st) return j;
    j["track_changes"] = d.track_changes();
    j["update_fields_on_open"] = (bool)st.child("w:updateFields");
    j["even_odd_headers"] = (bool)st.child("w:evenAndOddHeaders");
    j["mirror_margins"] = (bool)st.child("w:mirrorMargins");
    j["default_tab_stop"] = std::stod(xml::attr(st.child("w:defaultTabStop"), "w:val", "720")) / 20;
    j["zoom"] = std::stoi(xml::attr(st.child("w:zoom"), "w:percent", "100"));
    for (auto cs : xml::children(st.child("w:compat"), "w:compatSetting"))
        if (xml::attr(cs, "w:name") == "compatibilityMode") j["compatibility_mode"] = std::stoi(xml::attr(cs, "w:val", "15"));
    j["protection"] = protection_info(d);
    j["do_not_track_moves"] = (bool)st.child("w:doNotTrackMoves");
    j["do_not_track_formatting"] = (bool)st.child("w:doNotTrackFormatting");
    j["auto_hyphenation"] = (bool)st.child("w:autoHyphenation");
    j["embed_true_type_fonts"] = (bool)st.child("w:embedTrueTypeFonts");
    j["hide_spelling_errors"] = (bool)st.child("w:hideSpellingErrors");
    j["hide_grammar_errors"] = (bool)st.child("w:hideGrammaticalErrors");
    if (auto rv = st.child("w:revisionView")) {
        j["show_markup"] = xml::attr(rv, "w:markup", "1") != "0";
        j["show_formatting_changes"] = xml::attr(rv, "w:formatting", "1") != "0";
    }
    return j;
}

void set_settings(Document& d, const Json& props) {
    auto st = d.settings();
    auto toggle = [&](const char* q, bool on) {
        if (on) { auto e = schema::ensure_child(st, q); xml::remove_attr(e, "w:val"); }
        else st.remove_child(q);
    };
    for (auto& [k, v] : props.items()) {
        if (k == "track_changes") d.set_track_changes(v.get<bool>());
        else if (k == "update_fields_on_open") {
            if (v.get<bool>()) xml::set_attr(schema::ensure_child(st, "w:updateFields"), "w:val", "true");
            else st.remove_child("w:updateFields");
        }
        else if (k == "even_odd_headers") toggle("w:evenAndOddHeaders", v.get<bool>());
        else if (k == "mirror_margins") toggle("w:mirrorMargins", v.get<bool>());
        else if (k == "gutter_at_top") toggle("w:gutterAtTop", v.get<bool>());
        else if (k == "do_not_track_moves") toggle("w:doNotTrackMoves", v.get<bool>());
        else if (k == "do_not_track_formatting") toggle("w:doNotTrackFormatting", v.get<bool>());
        else if (k == "auto_hyphenation") toggle("w:autoHyphenation", v.get<bool>());
        else if (k == "embed_true_type_fonts") toggle("w:embedTrueTypeFonts", v.get<bool>());
        else if (k == "hide_spelling_errors") toggle("w:hideSpellingErrors", v.get<bool>());
        else if (k == "hide_grammar_errors") toggle("w:hideGrammaticalErrors", v.get<bool>());
        else if (k == "default_tab_stop") xml::set_attr(schema::ensure_child(st, "w:defaultTabStop"), "w:val", std::to_string(std::lround(v.get<double>() * 20)));
        else if (k == "zoom") xml::set_attr(schema::ensure_child(st, "w:zoom"), "w:percent", std::to_string(v.get<int>()));
        else if (k == "show_markup" || k == "show_formatting_changes" || k == "show_insertions_deletions" || k == "show_comments") {
            auto rv = schema::ensure_child(st, "w:revisionView");
            const char* a = k == "show_markup" ? "w:markup" : k == "show_formatting_changes" ? "w:formatting"
                          : k == "show_insertions_deletions" ? "w:insDel" : "w:comments";
            xml::set_attr(rv, a, v.get<bool>() ? "1" : "0");
        } else if (k == "compatibility_mode") {
            auto c = schema::ensure_child(st, "w:compat");
            pugi::xml_node cs;
            for (auto e : c.children("w:compatSetting")) if (xml::attr(e, "w:name") == "compatibilityMode") cs = e;
            if (!cs) {
                cs = c.append_child("w:compatSetting");
                xml::set_attr(cs, "w:name", "compatibilityMode");
                xml::set_attr(cs, "w:uri", "http://schemas.microsoft.com/office/word");
            }
            xml::set_attr(cs, "w:val", std::to_string(v.get<int>()));
        } else throw std::runtime_error("unknown setting '" + k + "'");
    }
}

// ── statistics ───────────────────────────────────────────────────────────

Json statistics(Document& d) {
    // Word counts main text + (by default) footnotes/endnotes/text boxes.
    long words = 0, chars = 0, chars_ns = 0, paras = 0, lines_est = 0;
    auto count = [&](pugi::xml_node root) {
        for (auto p : paragraphs(root)) {
            auto t = analyze(p).text;
            bool any = false;
            bool in_word = false;
            for (char32_t c : t) {
                if (c == kObj) continue;
                bool ws = c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == 0xA0;
                chars++;
                if (!ws) chars_ns++;
                if (!ws) any = true;
                if (!ws && !in_word) { words++; in_word = true; }
                if (ws) in_word = false;
            }
            if (any) paras++;
            lines_est += std::max<long>(any ? 1 : 0, (long)(t.size() / 90) + (any ? 1 : 0));
        }
    };
    count(d.body());
    if (auto fn = d.footnotes(false)) count(fn);
    if (auto en = d.endnotes(false)) count(en);
    Json j;
    j["words"] = words;
    j["characters_no_spaces"] = chars_ns;
    j["characters_with_spaces"] = chars;
    j["paragraphs"] = paras;
    j["lines_estimate"] = lines_est;
    j["pages_estimate"] = std::max<long>(1, (lines_est + 45) / 46);
    j["tables"] = tables(d).size();
    j["comments"] = list_comments(d).size();
    j["revisions"] = list_revisions(d).size();
    j["sections"] = sections(d).size();
    return j;
}

// ── reading ──────────────────────────────────────────────────────────────

namespace {

std::string view_text(pugi::xml_node p, const std::string& view) {
    if (view == "original") return util::to_utf8(original_text(p));
    return analyze(p).utf8();
}

// Markers for revisions/comments in a paragraph (compact, for the model).
Json para_marks(pugi::xml_node p) {
    Json m = Json::object();
    int ins = 0, del = 0, fmt = 0;
    for (auto n : xml::descendants(p, "w:ins")) if (std::string_view(n.parent().name()) != "w:rPr") ins++;
    for (auto n : xml::descendants(p, "w:del")) if (std::string_view(n.parent().name()) != "w:rPr") del++;
    fmt += (int)xml::descendants(p, "w:rPrChange").size() + (int)xml::descendants(p, "w:pPrChange").size();
    if (auto mr = p.child("w:pPr").child("w:rPr")) {
        if (mr.child("w:ins")) m["mark_inserted"] = true;
        if (mr.child("w:del")) m["mark_deleted"] = true;
    }
    if (ins) m["insertions"] = ins;
    if (del) m["deletions"] = del;
    if (fmt) m["format_changes"] = fmt;
    std::vector<std::string> cids;
    for (auto c : xml::descendants(p, "w:commentRangeStart")) cids.push_back(xml::attr(c, "w:id"));
    if (!cids.empty()) m["comments"] = cids;
    return m;
}

}  // namespace

Json read_paragraphs(Document& d, int offset, int limit, bool with_runs, const std::string& view) {
    Json out = Json::array();
    // flatten body: paragraphs including those in tables, with table position
    struct Item { pugi::xml_node p; std::string where; };
    std::vector<Item> items;
    int tbl_no = 0;
    std::function<void(pugi::xml_node, const std::string&)> walk = [&](pugi::xml_node c, const std::string& where) {
        for (auto b : blocks(c)) {
            if (std::string_view(b.name()) == "w:tbl") {
                int tn = ++tbl_no;
                int r = 0;
                for (auto tr : b.children("w:tr")) {
                    int col = 0;
                    for (auto tc : tr.children("w:tc")) {
                        walk(tc, (where.empty() ? "" : where + "/") + "table " + std::to_string(tn) + " r" + std::to_string(r + 1) + "c" + std::to_string(col + 1));
                        col++;
                    }
                    r++;
                }
                continue;
            }
            items.push_back({b, where});
        }
    };
    walk(d.body(), "");
    int total = (int)items.size();
    int start = offset < 0 ? std::max(0, total + offset) : std::max(0, offset - 1);
    int end = limit > 0 ? std::min(total, start + limit) : total;
    for (int i = start; i < end; i++) {
        auto p = items[i].p;
        Json j;
        j["n"] = i + 1;
        j["id"] = para_id(p);
        std::string sty = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
        if (!sty.empty()) j["style"] = sty;
        std::string label = list_label(d, p);
        if (!label.empty()) j["list"] = label;
        if (!items[i].where.empty()) j["in"] = items[i].where;
        j["text"] = view_text(p, view);
        if (p.child("w:pPr").child("w:sectPr")) j["section_break"] = true;
        auto m = para_marks(p);
        if (!m.empty()) j["marks"] = m;
        if (with_runs) {
            Json runs = Json::array();
            auto pt = analyze(p);
            pugi::xml_node last;
            std::u32string cur;
            int start_off = 0, k = 0;
            auto flush = [&](int endk) {
                if (!last) return;
                Json r;
                r["start"] = start_off;
                r["end"] = endk;
                r["text"] = util::to_utf8(cur);
                auto f = rpr_to_json(last.child("w:rPr"));
                if (!f.empty()) r["format"] = f;
                auto c = last.parent();
                if (is_revision_container(c)) {
                    r["revision"] = std::string(c.name()).substr(2);
                    r["author"] = xml::attr(c, "w:author");
                }
                runs.push_back(r);
            };
            for (int idx : pt.vis) {
                auto r = pt.atoms[idx].run;
                if (r != last) { flush(k); last = r; cur.clear(); start_off = k; }
                cur.push_back(pt.atoms[idx].ch);
                k++;
            }
            flush(k);
            j["runs"] = runs;
        }
        out.push_back(j);
    }
    Json res;
    res["total_paragraphs"] = total;
    res["paragraphs"] = out;
    if (end < total) res["next_offset"] = end + 1;
    return res;
}

Json outline(Document& d) {
    Json out = Json::array();
    for (auto p : paragraphs(d.body(), false)) {
        std::string sid = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
        int lvl = 0;
        if (auto o = p.child("w:pPr").child("w:outlineLvl")) lvl = std::stoi(xml::attr(o, "w:val", "9")) + 1;
        else {
            std::set<std::string> seen;
            for (std::string s = sid; !s.empty() && !seen.count(s);) {
                seen.insert(s);
                auto st = find_style(d, s);
                if (!st) break;
                if (auto o = st.child("w:pPr").child("w:outlineLvl")) { lvl = std::stoi(xml::attr(o, "w:val", "9")) + 1; break; }
                s = xml::attr(st.child("w:basedOn"), "w:val");
            }
        }
        if (lvl < 1 || lvl > 9) continue;
        std::string t = util::trim(analyze(p).utf8());
        if (t.empty()) continue;
        out.push_back({{"level", lvl}, {"text", t}, {"id", para_id(p)}, {"style", sid}});
    }
    return out;
}

std::string to_text(Document& d, const std::string& view, bool md) {
    std::string s;
    std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node c) {
        for (auto b : blocks(c)) {
            if (std::string_view(b.name()) == "w:tbl") {
                bool first = true;
                for (auto tr : b.children("w:tr")) {
                    std::string row = "|";
                    int cols = 0;
                    for (auto tc : tr.children("w:tc")) {
                        std::string cell;
                        for (auto p : blocks(tc))
                            if (std::string_view(p.name()) == "w:p") cell += (cell.empty() ? "" : " ") + view_text(p, view);
                        row += " " + cell + " |";
                        cols++;
                    }
                    s += row + "\n";
                    if (first && md) {
                        s += "|";
                        for (int i = 0; i < cols; i++) s += " --- |";
                        s += "\n";
                    }
                    first = false;
                }
                s += "\n";
                continue;
            }
            std::string t = view_text(b, view);
            if (md) {
                std::string sid = xml::attr(b.child("w:pPr").child("w:pStyle"), "w:val");
                if (sid == "Title") t = "# " + t;
                else if (util::starts_with(sid, "Heading") && sid.size() == 8 && std::isdigit((unsigned char)sid[7]))
                    t = std::string(sid[7] - '0' + (d.body() ? 1 : 0), '#') + " " + t;
                std::string label = list_label(d, b);
                if (!label.empty()) {
                    int lvl = std::stoi(xml::attr(b.child("w:pPr").child("w:numPr").child("w:ilvl"), "w:val", "0"));
                    t = std::string(lvl * 2, ' ') + (label == "\u2022" || label == "o" || label == "\u25AA" ? "-" : label) + " " + t;
                }
            }
            s += t + "\n";
        }
    };
    walk(d.body());
    return s;
}

// ── compare ──────────────────────────────────────────────────────────────

namespace {

std::vector<std::u32string> tokenize(const std::u32string& s) {
    // words, whitespace runs and punctuation as separate tokens (like Word's word-level compare)
    std::vector<std::u32string> v;
    std::u32string cur;
    int kind = -1;
    auto k_of = [](char32_t c) {
        if (c == ' ' || c == '\t' || c == 0xA0) return 0;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '\'' || c >= 0xC0) return 1;
        return 2;
    };
    for (char32_t c : s) {
        int k = k_of(c);
        if (k == 2 || k != kind) {
            if (!cur.empty()) v.push_back(cur);
            cur.clear();
        }
        cur.push_back(c);
        kind = k;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

// LCS over sequences; returns ops: 0 equal, 1 delete (a), 2 insert (b).
template <class T, class Eq>
std::vector<std::pair<int, std::pair<int, int>>> diff(const std::vector<T>& a, const std::vector<T>& b, Eq eq) {
    int n = (int)a.size(), m = (int)b.size();
    // trim common prefix/suffix for speed
    int pre = 0;
    while (pre < n && pre < m && eq(a[pre], b[pre])) pre++;
    int suf = 0;
    while (suf < n - pre && suf < m - pre && eq(a[n - 1 - suf], b[m - 1 - suf])) suf++;
    int N = n - pre - suf, M = m - pre - suf;
    std::vector<std::vector<int>> L(N + 1, std::vector<int>(M + 1, 0));
    for (int i = N - 1; i >= 0; i--)
        for (int j = M - 1; j >= 0; j--)
            L[i][j] = eq(a[pre + i], b[pre + j]) ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
    std::vector<std::pair<int, std::pair<int, int>>> ops;
    for (int i = 0; i < pre; i++) ops.push_back({0, {i, i}});
    int i = 0, j = 0;
    while (i < N || j < M) {
        if (i < N && j < M && eq(a[pre + i], b[pre + j])) { ops.push_back({0, {pre + i, pre + j}}); i++; j++; }
        else if (j < M && (i == N || L[i][j + 1] >= L[i + 1][j])) { ops.push_back({2, {-1, pre + j}}); j++; }
        else { ops.push_back({1, {pre + i, -1}}); i++; }
    }
    for (int k = 0; k < suf; k++) ops.push_back({0, {n - suf + k, m - suf + k}});
    return ops;
}

double similarity(const std::u32string& a, const std::u32string& b) {
    if (a == b) return 1;
    auto ta = tokenize(a), tb = tokenize(b);
    if (ta.empty() || tb.empty()) return 0;
    auto ops = diff(ta, tb, [](auto& x, auto& y) { return x == y; });
    int same = 0;
    for (auto& o : ops) if (o.first == 0) same++;
    return 2.0 * same / double(ta.size() + tb.size());
}

}  // namespace

namespace {

struct Cmp {
    Document& out;
    double thresh;

    static std::u32string sig(pugi::xml_node b) {
        if (std::string_view(b.name()) == "w:p") return analyze(b).text;
        // a table's signature is its text, so equal tables match exactly
        std::u32string s = U"\uFFFF";
        for (auto p : paragraphs(b)) s += analyze(p).text + U"\u00B6";
        return s;
    }
    static bool is_p(pugi::xml_node b) { return std::string_view(b.name()) == "w:p"; }

    void container(pugi::xml_node ocont, pugi::xml_node rcont);
    void table(pugi::xml_node ot, pugi::xml_node rt);
    void paragraph(pugi::xml_node o, pugi::xml_node r);
    void mark_inserted(pugi::xml_node b);
    void copy_deleted(pugi::xml_node ob, pugi::xml_node parent, pugi::xml_node before);
    void mark_row(pugi::xml_node tr, const char* q);
};

}  // namespace

Document compare(Document& original, Document& revised, const std::string& author, const Json& opts_in) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    // Start from the revised document so styles/sections/media match it, and
    // reconstruct the original text as tracked deletions.
    Document out = Document::from_bytes(revised.to_bytes());
    out.author.name = author.empty() ? "Compare" : author;
    out.author.date = util::now_iso8601();
    Cmp c{out, opts.value("paragraph_match", 0.5)};
    c.container(original.body(), out.body());
    // ids from the original copy must stay unique
    std::set<std::string> seen;
    for (auto p : paragraphs(out.body())) {
        std::string id = util::lower(xml::attr(p, "w14:paraId"));
        if (id.empty() || seen.count(id)) xml::set_attr(p, "w14:paraId", out.new_hex_id());
        seen.insert(util::lower(xml::attr(p, "w14:paraId")));
    }
    xml::declare(out.doc_xml().document_element(), "w14", true);
    return out;
}

void Cmp::container(pugi::xml_node ocont, pugi::xml_node rcont) {
    auto rblocks = blocks(rcont);
    auto oblocks = blocks(ocont);
    std::vector<std::u32string> ot, rt;
    for (auto b : oblocks) ot.push_back(sig(b));
    for (auto b : rblocks) rt.push_back(sig(b));
    // block-level diff; similar paragraphs (or any two tables) pair up as "changed"
    auto ops = diff(ot, rt, [](auto& x, auto& y) { return x == y; });
    std::vector<std::pair<int, std::pair<int, int>>> merged;
    for (std::size_t k = 0; k < ops.size();) {
        if (ops[k].first == 0) { merged.push_back(ops[k]); k++; continue; }
        std::vector<int> dels, ins;
        while (k < ops.size() && ops[k].first != 0) {
            if (ops[k].first == 1) dels.push_back(ops[k].second.first);
            else ins.push_back(ops[k].second.second);
            k++;
        }
        // pair in order when similar
        std::size_t di = 0, ii = 0;
        while (di < dels.size() || ii < ins.size()) {
            bool pair = false;
            if (di < dels.size() && ii < ins.size()) {
                auto ob = oblocks[dels[di]], rb = rblocks[ins[ii]];
                if (is_p(ob) && is_p(rb)) pair = similarity(ot[dels[di]], rt[ins[ii]]) >= thresh;
                else if (!is_p(ob) && !is_p(rb)) pair = true;
            }
            if (pair) {
                merged.push_back({3, {dels[di], ins[ii]}});  // modified block
                di++; ii++;
            } else if (di < dels.size() && (ii >= ins.size() || di <= ii)) {
                merged.push_back({1, {dels[di], -1}}); di++;
            } else {
                merged.push_back({2, {-1, ins[ii]}}); ii++;
            }
        }
    }
    // inserted/modified blocks are marked in place; deleted original blocks
    // are copied in before the next surviving revised block
    std::vector<std::pair<pugi::xml_node, pugi::xml_node>> deletions;
    for (std::size_t k = 0; k < merged.size(); k++) {
        auto [op, ij] = merged[k];
        if (op == 1) {
            pugi::xml_node next;
            for (std::size_t q = k + 1; q < merged.size(); q++)
                if (merged[q].second.second >= 0) { next = rblocks[merged[q].second.second]; break; }
            deletions.push_back({oblocks[ij.first], next});
        } else if (op == 2) {
            mark_inserted(rblocks[ij.second]);
        } else {
            auto ob = oblocks[ij.first], rb = rblocks[ij.second];
            if (is_p(ob) && is_p(rb)) { if (op == 3) paragraph(ob, rb); }
            else if (!is_p(ob) && !is_p(rb)) table(ob, rb);
        }
    }
    for (auto& [ob, next] : deletions) {
        pugi::xml_node before = next ? next : rcont.child("w:sectPr");
        copy_deleted(ob, next ? next.parent() : rcont, before);
    }
}

void Cmp::mark_row(pugi::xml_node tr, const char* q) {
    stamp(out, schema::ensure_child(schema::ensure_child(tr, "w:trPr"), q));
}

void Cmp::table(pugi::xml_node otbl, pugi::xml_node rtbl) {
    auto orows = xml::children(otbl, "w:tr"), rrows = xml::children(rtbl, "w:tr");
    auto rowsig = [](pugi::xml_node tr) { return sig(tr); };
    std::vector<std::u32string> os, rs;
    for (auto r : orows) os.push_back(rowsig(r));
    for (auto r : rrows) rs.push_back(rowsig(r));
    auto ops = diff(os, rs, [](auto& x, auto& y) { return x == y; });
    // pair changed rows with the same cell count
    std::vector<std::pair<int, std::pair<int, int>>> merged;
    for (std::size_t k = 0; k < ops.size();) {
        if (ops[k].first == 0) { merged.push_back(ops[k]); k++; continue; }
        std::vector<int> dels, ins;
        while (k < ops.size() && ops[k].first != 0) {
            if (ops[k].first == 1) dels.push_back(ops[k].second.first);
            else ins.push_back(ops[k].second.second);
            k++;
        }
        std::size_t di = 0, ii = 0;
        while (di < dels.size() || ii < ins.size()) {
            if (di < dels.size() && ii < ins.size() &&
                xml::children(orows[dels[di]], "w:tc").size() == xml::children(rrows[ins[ii]], "w:tc").size()) {
                merged.push_back({3, {dels[di], ins[ii]}});
                di++; ii++;
            } else if (di < dels.size() && (ii >= ins.size() || di <= ii)) {
                merged.push_back({1, {dels[di], -1}}); di++;
            } else {
                merged.push_back({2, {-1, ins[ii]}}); ii++;
            }
        }
    }
    std::vector<std::pair<pugi::xml_node, pugi::xml_node>> deleted_rows;
    for (std::size_t k = 0; k < merged.size(); k++) {
        auto [op, ij] = merged[k];
        if (op == 0) continue;
        if (op == 2) {
            auto tr = rrows[ij.second];
            mark_row(tr, "w:ins");
            for (auto tc : tr.children("w:tc")) for (auto b : blocks(tc)) mark_inserted(b);
        } else if (op == 1) {
            pugi::xml_node next;
            for (std::size_t q = k + 1; q < merged.size(); q++)
                if (merged[q].second.second >= 0) { next = rrows[merged[q].second.second]; break; }
            deleted_rows.push_back({orows[ij.first], next});
        } else {
            auto oc = xml::children(orows[ij.first], "w:tc"), rc = xml::children(rrows[ij.second], "w:tc");
            for (std::size_t i = 0; i < oc.size(); i++) container(oc[i], rc[i]);
        }
    }
    for (auto& [otr, next] : deleted_rows) {
        auto ntr = next ? rtbl.insert_copy_before(otr, next) : rtbl.append_copy(otr);
        mark_row(ntr, "w:del");
        for (auto tc : ntr.children("w:tc")) {
            std::vector<pugi::xml_node> bs = blocks(tc);
            for (auto b : bs) {
                // reuse the deletion marking on the copied content in place
                auto parent = b.parent();
                auto after = b.next_sibling();
                pugi::xml_document tmp;
                tmp.append_copy(b);
                parent.remove_child(b);
                copy_deleted(tmp.first_child(), parent, after);
            }
        }
        if (!ntr.attribute("w14:paraId")) ntr.prepend_attribute("w14:paraId") = out.new_hex_id().c_str();
        else xml::set_attr(ntr, "w14:paraId", out.new_hex_id());
    }
    // table/row/cell property changes are left as they are in the revised
    // document (Word reports them as formatting changes only on request)
}

void Cmp::paragraph(pugi::xml_node o, pugi::xml_node r) {
    {
        auto a = tokenize(analyze(o).text), b = tokenize(analyze(r).text);
        auto wops = diff(a, b, [](auto& x, auto& y) { return x == y; });
        // walk revised text positions; deleted original words get re-inserted as deleted runs
        int pos = 0;
        std::vector<std::pair<int, std::u32string>> to_delete_insert;
        std::vector<std::pair<int, int>> ins_ranges;
        for (auto& w : wops) {
            if (w.first == 0) pos += (int)b[w.second.second].size();
            else if (w.first == 2) { ins_ranges.push_back({pos, pos + (int)b[w.second.second].size()}); pos += (int)b[w.second.second].size(); }
            else {
                if (!to_delete_insert.empty() && to_delete_insert.back().first == pos) to_delete_insert.back().second += a[w.second.first];
                else to_delete_insert.push_back({pos, a[w.second.first]});
            }
        }
        // mark insertions (back to front)
        for (auto it = ins_ranges.rbegin(); it != ins_ranges.rend(); ++it) {
            auto runs = isolate(r, it->first, it->second);
            for (auto run : runs) {
                if (std::string_view(run.name()) != "w:r") continue;
                auto ins = run.parent().insert_child_before("w:ins", run);
                stamp(out, ins);
                ins.append_move(run);
            }
        }
        // add deleted text at positions, in a run with the formatting of the neighbouring text
        for (auto it = to_delete_insert.rbegin(); it != to_delete_insert.rend(); ++it) {
            int p0 = it->first;
            split_at(r, p0);
            ParaText pt = analyze(r);
            pugi::xml_node parent = r, before;
            pugi::xml_node fmt;
            int n = (int)pt.vis.size();
            if (p0 < n) {
                auto node = pt.atoms[pt.vis[p0]].run;
                fmt = node;
                while (is_revision_container(node.parent())) node = node.parent();
                parent = node.parent();
                before = node;
            } else if (n > 0) {
                auto node = pt.atoms[pt.vis[n - 1]].run;
                fmt = node;
                while (is_revision_container(node.parent())) node = node.parent();
                parent = node.parent();
                before = node.next_sibling();
            }
            auto del = before ? parent.insert_child_before("w:del", before) : parent.append_child("w:del");
            stamp(out, del);
            make_run(del, {}, fmt ? fmt.child("w:rPr") : pugi::xml_node{}, it->second, true);
        }
        // paragraph formatting change
        auto op = o.child("w:pPr"), rp = r.child("w:pPr");
        auto strip = [](pugi::xml_node pr) {
            std::string s;
            if (!pr) return s;
            for (auto c : pr.children()) {
                std::string_view q = c.name();
                if (q == "w:rPr" || q == "w:sectPr" || q == "w:pPrChange") continue;
                s += xml::to_string(c);
            }
            return s;
        };
        if (strip(op) != strip(rp)) {
            auto ppr = schema::ensure_child(r, "w:pPr");
            auto ch = schema::ensure_child(ppr, "w:pPrChange");
            stamp(out, ch);
            auto old = ch.append_child("w:pPr");
            if (op)
                for (auto c : op.children()) {
                    std::string_view q = c.name();
                    if (q == "w:rPr" || q == "w:sectPr" || q == "w:pPrChange") continue;
                    old.append_copy(c);
                }
        }
        tidy(r);
    }
}

namespace {
// Track a whole paragraph as inserted (q = w:ins) or deleted (w:del): the
// mark, every run-level child, and math runs (via w:rPr markers).
void track_paragraph(Document& out, pugi::xml_node p, const char* q) {
    bool del = std::string_view(q) == "w:del";
    if (del)
        for (auto r : xml::descendants(p, "w:r"))
            for (auto t : r.children()) {
                if (std::string_view(t.name()) == "w:t") t.set_name("w:delText");
                if (std::string_view(t.name()) == "w:instrText") t.set_name("w:delInstrText");
            }
    std::vector<pugi::xml_node> kids;
    for (auto c : p.children()) {
        std::string_view n = c.name();
        if (n == "w:r" || n == "w:hyperlink" || n == "w:fldSimple" || n == "w:smartTag" || n == "w:sdt") kids.push_back(c);
    }
    for (auto c : kids) {
        auto w = p.insert_child_before(q, c);
        stamp(out, w);
        w.append_move(c);
    }
    for (auto mr : xml::descendants(p, "m:r")) wrap_math_run(out, mr, q);
    auto mrpr = schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:rPr");
    if (!mrpr.child(q)) stamp(out, schema::ensure_child(mrpr, q));
}
}  // namespace

void Cmp::mark_inserted(pugi::xml_node b) {
    if (is_p(b)) { track_paragraph(out, b, "w:ins"); return; }
    for (auto trn : b.children("w:tr")) stamp(out, schema::ensure_child(schema::ensure_child(trn, "w:trPr"), "w:ins"));
    for (auto p : xml::descendants(b, "w:p")) track_paragraph(out, p, "w:ins");
}

void Cmp::copy_deleted(pugi::xml_node ob, pugi::xml_node parent, pugi::xml_node before) {
    pugi::xml_node nb = before ? parent.insert_copy_before(ob, before) : parent.append_copy(ob);
    if (is_p(nb)) nb.child("w:pPr").remove_child("w:sectPr");
    // drop things that point into the original's parts (comments, notes, media, links)
    for (auto q : {"w:commentRangeStart", "w:commentRangeEnd", "w:commentReference", "w:footnoteReference", "w:endnoteReference"})
        for (auto e : xml::descendants(nb, q)) e.parent().remove_child(e);
    for (auto dr : xml::descendants(nb, "w:drawing")) dr.parent().remove_child(dr);
    for (auto h : xml::descendants(nb, "w:hyperlink")) h.remove_attribute("r:id");
    if (!is_p(nb))
        for (auto trn : nb.children("w:tr")) stamp(out, schema::ensure_child(schema::ensure_child(trn, "w:trPr"), "w:del"));
    for (auto p : is_p(nb) ? std::vector<pugi::xml_node>{nb} : xml::descendants(nb, "w:p")) track_paragraph(out, p, "w:del");
    out.ensure_para_ids(nb);
}

}  // namespace docx::wml

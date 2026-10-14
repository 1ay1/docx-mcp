// Accept/reject every revision in the RP corpus and compare with the
// expected files (Word/PowerTools output): paragraph texts, paragraph props,
// run formatting, table shape. Prints mismatches; exit 1 if any.
#include <cstdio>
#include <filesystem>
#include <string>

#include "schema/schema.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace fs = std::filesystem;
using namespace docx;

namespace {

bool g_ignore_ppr = false;

std::string ppr_sig(pugi::xml_node p) {
    auto ppr = p.child("w:pPr");
    if (!ppr || g_ignore_ppr) return "";
    std::string s;
    for (auto c : ppr.children()) {
        std::string_view q = c.name();
        if (q == "w:rPr" || q == "w:sectPr") continue;
        s += xml::to_string(c);
    }
    return s;
}

std::string run_sig(pugi::xml_node p) {
    // formatting of each visible character run, merged
    std::string s;
    auto pt = wml::analyze(p);
    std::string last;
    for (int i : pt.vis) {
        auto r = pt.atoms[i].run;
        auto rpr = r.child("w:rPr");
        std::string f = rpr && !g_ignore_ppr ? xml::to_string(rpr) : "";
        if (f != last) { s += "|" + f; last = f; }
    }
    return s;
}

// Shape of a story: blocks in order with texts.
std::vector<std::string> shape(pugi::xml_node root) {
    std::vector<std::string> v;
    std::function<void(pugi::xml_node, int)> walk = [&](pugi::xml_node n, int depth) {
        for (auto b : wml::blocks(n)) {
            if (std::string_view(b.name()) == "w:tbl") {
                int rows = 0;
                for (auto tr : b.children("w:tr")) {
                    rows++;
                    int cells = 0;
                    for (auto tc : tr.children("w:tc")) {
                        cells++;
                        walk(tc, depth + 1);
                    }
                    v.push_back(std::string(depth, ' ') + "[row " + std::to_string(rows) + " cells=" + std::to_string(cells) + "]");
                }
                continue;
            }
            v.push_back(std::string(depth, ' ') + "P{" + ppr_sig(b) + "} " + wml::analyze(b).utf8() + " R" + run_sig(b));
        }
    };
    walk(root, 0);
    return v;
}

int compare(const std::string& got_label, wml::Document& got, wml::Document& want) {
    auto a = shape(got.body()), b = shape(want.body());
    if (a == b) return 0;
    std::printf("MISMATCH %s\n", got_label.c_str());
    std::size_t n = std::max(a.size(), b.size());
    int shown = 0;
    for (std::size_t i = 0; i < n && shown < 4; i++) {
        std::string x = i < a.size() ? a[i] : "<none>", y = i < b.size() ? b[i] : "<none>";
        if (x == y) continue;
        std::printf("  got : %.400s\n  want: %.400s\n", x.c_str(), y.c_str());
        shown++;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/corpus/oxpt/RP";
    int bad = 0, total = 0;
    std::vector<fs::path> bases;
    for (auto& e : fs::directory_iterator(dir)) {
        auto n = e.path().filename().string();
        if (n.ends_with("-Accepted.docx") || n.ends_with("-Rejected.docx")) continue;
        bases.push_back(e.path());
    }
    std::sort(bases.begin(), bases.end());
    for (auto& base : bases) {
        std::string stem = base.string().substr(0, base.string().size() - 5);
        for (bool acc : {true, false}) {
            std::string exp = stem + (acc ? "-Accepted.docx" : "-Rejected.docx");
            if (!fs::exists(exp)) continue;
            total++;
            try {
                auto d = wml::Document::open(base.string());
                wml::RevFilter f;
                if (acc) wml::accept_revisions(d, f);
                else wml::reject_revisions(d, f);
                auto bytes = d.to_bytes();
                auto re = wml::Document::from_bytes(bytes);
                auto want = wml::Document::open(exp);
                // Some expected files are PowerTools intermediates (pt14 markup,
                // pPr partly stripped); compare only their text and shape.
                g_ignore_ppr = xml::to_string(want.body()).find("pt14:") != std::string::npos ||
                               !want.body().find_node([](pugi::xml_node n) { return std::string_view(n.name()) == "w:pPr"; });
                int m = compare(base.filename().string() + (acc ? " accept" : " reject"), re, want);
                // Expected files that are PowerTools artifacts:
                //  RP015: PowerTools leaves an empty paragraph for a moved paragraph
                //         mark; ECMA 17.13.5.22/23 say the mark is removed.
                //  RP051 reject: expected keeps a w:tbl with no rows, which is
                //         schema-invalid; we remove the table.
                std::string fn = base.filename().string();
                if (m && (fn == "RP015-MoveFrom-MoveTo.docx" || (fn == "RP051-Arabic.docx" && !acc))) {
                    std::printf("  (known fixture deviation, not counted)\n");
                    m = 0;
                }
                // and no revision markup may remain
                auto left = wml::list_revisions(re);
                if (!left.empty()) {
                    std::printf("LEFTOVER %s %s: %zu revisions (%s)\n", base.filename().c_str(), acc ? "accept" : "reject",
                                left.size(), left[0].type.c_str());
                    m = 1;
                }
                // schema validity of the result
                auto issues = schema::validate(re.doc_xml().document_element());
                if (!issues.empty()) {
                    std::printf("INVALID %s %s: %s :: %s\n", base.filename().c_str(), acc ? "accept" : "reject",
                                issues[0].path.c_str(), issues[0].message.c_str());
                    m = 1;
                }
                bad += m;
            } catch (std::exception& e) {
                std::printf("ERROR %s: %s\n", base.filename().c_str(), e.what());
                bad++;
            }
        }
    }
    std::printf("%d/%d match\n", total - bad, total);
    return bad ? 1 : 0;
}

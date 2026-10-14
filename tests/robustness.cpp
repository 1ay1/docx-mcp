// Robustness: run a battery of edits on every corpus document. The edited
// file must open again, keep accept/reject working, and introduce no new
// schema issues (pre-existing issues in Word's own files are tolerated).
#include <cstdio>
#include <filesystem>
#include <string>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/comments.hpp"
#include "wml/docops.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace fs = std::filesystem;
using namespace docx;
using namespace docx::wml;

namespace {
int issues(Document& d, std::string* first = nullptr) {
    int n = 0;
    for (auto& part : d.pkg().part_names()) {
        if (schema::part_root_type(d.pkg().content_type_of(part)) < 0) continue;
        auto v = schema::validate(d.pkg().xml(part).document_element(), {100000});
        if (!v.empty() && first && first->empty()) *first = part + " " + v[0].path + " :: " + v[0].message;
        n += (int)v.size();
    }
    return n;
}

// Pick a paragraph with some text in the main body.
pugi::xml_node pick(Document& d, int nth) {
    int k = 0;
    for (auto p : paragraphs(d.body(), false))
        if (analyze(p).vis.size() > 12 && k++ == nth) return p;
    return {};
}
}  // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/corpus/oxpt";
    int total = 0, bad = 0;
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(dir))
        if (e.path().extension() == ".docx") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        total++;
        std::string stage = "open";
        try {
            auto d = Document::open(f.string());
            int base = issues(d);
            d.author.name = "Robot";
            stage = "edits";
            for (bool track : {true, false}) {
                EditOpts o{track};
                if (auto p = pick(d, 0)) {
                    insert_text(d, p, 3, U"[ins]", nullptr, o);
                    delete_text(d, p, 0, 2, o);
                    format_runs(d, p, 4, 8, {{"bold", true}, {"color", "C00000"}}, o);
                    format_paragraph(d, p, {{"space_after", 6}}, o);
                    add_comment(d, p, 1, 5, "robot comment");
                }
                if (auto p = pick(d, 1)) {
                    replace_text(d, p, 2, 6, U"REPL", nullptr, o);
                    auto np = insert_paragraph(d, p, true, U"New para\nSecond new", "", nullptr, o);
                    split_paragraph(d, np, 3, o);
                }
                if (auto p = pick(d, 2)) delete_paragraph(d, p, o);
                FindOpts fo;
                fo.max = 3;
                replace_all(d, "the", "THE", fo, nullptr, o);
            }
            stage = "structure";
            if (auto p = pick(d, 0)) {
                insert_table(d, p, true, {{"a", "b"}, {"c", "d"}}, nullptr, {});
                apply_list(d, {p}, "number", 0, nullptr, {});
                add_bookmark(d, p, 0, p, 3, "robot_bm");
                add_hyperlink(d, p, 0, 0, "https://example.com", "link ", "", {});
                add_note(d, p, 2, "footnote", "robot note", {});
            }
            stage = "save";
            auto bytes = d.to_bytes();
            if (const char* out = std::getenv("ROBUST_OUT")) {
                fs::create_directories(out);
                std::FILE* fp = std::fopen((std::string(out) + "/" + f.filename().string()).c_str(), "wb");
                std::fwrite(bytes.data(), 1, bytes.size(), fp);
                std::fclose(fp);
            }
            auto re = Document::from_bytes(bytes);
            std::string why;
            int after = issues(re, &why);
            stage = "accept";
            auto a = Document::from_bytes(bytes);
            accept_revisions(a, {});
            std::string wa;
            int ai = issues(a, &wa);
            stage = "reject";
            auto r = Document::from_bytes(bytes);
            reject_revisions(r, {});
            std::string wr;
            int ri = issues(r, &wr);
            // accept/reject resolve the pre-existing tracked markup too, so
            // compare against the original's issue count
            if (after > base || ai > base || ri > base) {
                bad++;
                std::printf("NEW ISSUES %s (base %d, edited %d, accepted %d, rejected %d)\n  %s\n  %s\n  %s\n",
                            f.filename().string().c_str(), base, after, ai, ri, why.c_str(), wa.c_str(), wr.c_str());
            }
        } catch (std::exception& e) {
            bad++;
            std::printf("ERROR %s at %s: %s\n", f.filename().string().c_str(), stage.c_str(), e.what());
        }
    }
    std::printf("%d/%d documents clean\n", total - bad, total);
    return bad ? 1 : 0;
}

// Document ops: protection (verified against a Word-protected file),
// properties, settings, statistics, reading, compare.
#include <filesystem>

#include "schema/schema.hpp"
#include "t.hpp"
#include "util/util.hpp"
#include "wml/comments.hpp"
#include "wml/docops.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/structure.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

using namespace docx;
using namespace docx::wml;

namespace {
int schema_issues(Document& d, std::string* first = nullptr) {
    int n = 0;
    for (auto& part : d.pkg().part_names()) {
        if (schema::part_root_type(d.pkg().content_type_of(part)) < 0) continue;
        auto& x = d.pkg().xml(part);
        xml::normalize(x);
        auto issues = schema::validate(x.document_element());
        if (!issues.empty() && first && first->empty()) *first = part + " " + issues[0].path + " :: " + issues[0].message;
        n += (int)issues.size();
    }
    return n;
}
void save(Document& d, const std::string& name) {
    std::filesystem::create_directories("out");
    auto b = d.to_bytes();
    std::FILE* f = std::fopen(("out/" + name + ".docx").c_str(), "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    auto re = Document::from_bytes(b);
    std::string why;
    if (schema_issues(re, &why)) t::fail(name.c_str(), 0, "schema: " + why);
}
std::string joined(Document& d) {
    std::string s;
    for (auto p : paragraphs(d.body(), false)) s += analyze(p).utf8() + "\n";
    return s;
}
Document make(const std::vector<std::u32string>& lines) {
    auto d = Document::create();
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, lines[0], nullptr, {false});
    for (std::size_t i = 1; i < lines.size(); i++) p = insert_paragraph(d, p, true, lines[i], "", nullptr, {false});
    return d;
}
}  // namespace

TEST(protection_word_file) {
    // Word-protected by Word with password "Example" (Apache POI bug56076)
    auto d = Document::open(DOCX_CORPUS "/poi/bug56076.docx");
    CHECK(check_password(d, "Example"));
    CHECK(!check_password(d, "example"));
    CHECK(!check_password(d, "Wrong"));
    auto pi = protection_info(d);
    CHECK(pi["protected"].get<bool>());
    CHECK_EQ(pi["mode"].get<std::string>(), std::string("trackedChanges"));
}

TEST(protection_roundtrip) {
    auto d = make({U"Secret stuff."});
    protect(d, "readOnly", "hunter2");
    save(d, "protected");
    auto re = Document::from_bytes(d.to_bytes());
    CHECK(check_password(re, "hunter2"));
    CHECK(!check_password(re, "hunter3"));
    bool threw = false;
    try { unprotect(re, "nope"); } catch (std::exception&) { threw = true; }
    CHECK(threw);
    unprotect(re, "hunter2");
    CHECK(!protection_info(re)["protected"].get<bool>());
    // an empty password protects without one
    protect(re, "comments", "");
    CHECK(check_password(re, "anything"));
}

TEST(properties_and_settings) {
    auto d = make({U"x"});
    set_core_properties(d, {{"title", "Quarterly Report"}, {"subject", "Q3"}, {"keywords", "finance, q3"}, {"category", "Reports"}});
    set_custom_property(d, "Client", "Acme");
    set_custom_property(d, "Budget", 125000);
    set_custom_property(d, "Approved", true);
    auto cp = core_properties(d);
    CHECK_EQ(cp["title"].get<std::string>(), std::string("Quarterly Report"));
    CHECK_EQ(cp["category"].get<std::string>(), std::string("Reports"));
    auto cu = custom_properties(d);
    CHECK_EQ(cu["Client"].get<std::string>(), std::string("Acme"));
    CHECK_EQ(cu["Budget"].get<long long>(), 125000LL);
    CHECK(cu["Approved"].get<bool>());
    set_custom_property(d, "Client", nullptr);
    CHECK(!custom_properties(d).contains("Client"));
    set_settings(d, {{"track_changes", true}, {"default_tab_stop", 36}, {"even_odd_headers", true}, {"zoom", 120}});
    auto s = settings_json(d);
    CHECK(s["track_changes"].get<bool>());
    CHECK_EQ(s["zoom"].get<int>(), 120);
    save(d, "props_settings");
}

TEST(statistics_count) {
    auto d = make({U"The quick brown fox.", U"Jumps over  the dog."});
    auto st = statistics(d);
    CHECK_EQ(st["words"].get<long>(), 8L);
    CHECK_EQ(st["paragraphs"].get<long>(), 2L);
    CHECK_EQ(st["characters_no_spaces"].get<long>(), 33L);
}

TEST(read_and_outline) {
    auto d = make({U"Title here", U"Body text one.", U"Body two."});
    format_paragraph(d, paragraphs(d.body())[0], {{"style", "Heading1"}}, {false});
    d.set_track_changes(true);
    insert_text(d, paragraphs(d.body())[1], 0, U"New ", nullptr, {});
    auto r = read_paragraphs(d, 1, 0, true, "current");
    CHECK_EQ(r["total_paragraphs"].get<int>(), 3);
    CHECK_EQ(r["paragraphs"][1]["text"].get<std::string>(), std::string("New Body text one."));
    CHECK(r["paragraphs"][1]["marks"]["insertions"].get<int>() == 1);
    auto orig = read_paragraphs(d, 2, 1, false, "original");
    CHECK_EQ(orig["paragraphs"][0]["text"].get<std::string>(), std::string("Body text one."));
    auto o = outline(d);
    CHECK_EQ(o.size(), std::size_t(1));
    auto md = to_text(d, "current", true);
    CHECK(md.find("# Title here") != std::string::npos);
}

TEST(compare_documents) {
    auto a = make({U"The quick brown fox jumps.", U"A paragraph that will be removed.", U"Shared ending here."});
    auto b = make({U"The quick red fox leaps.", U"A brand new paragraph.", U"Shared ending here."});
    auto c = compare(a, b, "Comparer", {});
    save(c, "compare");
    // accept-all = revised, reject-all = original
    auto acc = Document::from_bytes(c.to_bytes());
    accept_revisions(acc, {});
    CHECK_EQ(joined(acc), joined(b));
    auto rej = Document::from_bytes(c.to_bytes());
    reject_revisions(rej, {});
    CHECK_EQ(joined(rej), joined(a));
    std::string w1, w2;
    CHECK_EQ(schema_issues(acc, &w1), 0);
    CHECK_EQ(schema_issues(rej, &w2), 0);
    auto revs = list_revisions(c);
    CHECK(!revs.empty());
    for (auto& r : revs) CHECK_EQ(r.author, std::string("Comparer"));
}

TEST(compare_corpus_pairs) {
    // the WmlComparer corpus: compare(X, X-Mod) must accept to X-Mod and reject to X
    namespace fs = std::filesystem;
    int n = 0, ok = 0;
    for (auto& e : fs::directory_iterator(DOCX_CORPUS "/oxpt/WC")) {
        auto name = e.path().string();
        if (!name.ends_with("-Mod.docx")) continue;
        std::string base = name.substr(0, name.size() - 9) + ".docx";
        if (!fs::exists(base)) continue;
        n++;
        try {
            auto a = Document::open(base), b = Document::open(name);
            // compare works on the current view; flatten any existing revisions
            accept_revisions(a, {});
            accept_revisions(b, {});
            auto c = compare(a, b, "Comparer", {});
            auto acc = Document::from_bytes(c.to_bytes());
            accept_revisions(acc, {});
            auto rej = Document::from_bytes(c.to_bytes());
            reject_revisions(rej, {});
            std::string w;
            int pre = schema_issues(a) + schema_issues(b);  // invalid markup inherited from the inputs
            int got = schema_issues(c, &w);
            bool text_ok = joined(acc) == joined(b) && joined(rej) == joined(a);
            bool good = text_ok && got <= pre && schema_issues(acc) <= pre && schema_issues(rej) <= pre;
            if (good) ok++;
            else {
                std::printf("    compare mismatch: %s %s\n", e.path().filename().string().c_str(), w.c_str());
                if (!text_ok) {
                    auto first_diff = [](const std::string& x, const std::string& y) {
                        auto lx = util::split(x, '\n'), ly = util::split(y, '\n');
                        for (std::size_t i = 0; i < std::max(lx.size(), ly.size()); i++) {
                            std::string sx = i < lx.size() ? lx[i] : "<none>", sy = i < ly.size() ? ly[i] : "<none>";
                            if (sx != sy) { std::printf("      line %zu: got [%.200s] want [%.200s]\n", i, sx.c_str(), sy.c_str()); return; }
                        }
                    };
                    if (joined(acc) != joined(b)) { std::printf("      accept differs\n"); first_diff(joined(acc), joined(b)); }
                    if (joined(rej) != joined(a)) { std::printf("      reject differs\n"); first_diff(joined(rej), joined(a)); }
                }
            }
        } catch (std::exception& ex) {
            std::printf("    compare error: %s: %s\n", e.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("    compare corpus: %d/%d\n", ok, n);
    CHECK_EQ(ok, n);
}

// Word-authored files with faults the schema rejects come out valid.
TEST(repair_on_open) {
    for (auto f : {"oxpt/HC032-Named-Color.docx", "oxpt/HC002-Hebrew-01.docx", "oxpt/WC/WC004-Large.docx",
                   "oxpt/RP/RP013-Deleted-Math-Control-Char.docx", "oxpt/RP/RP014-Inserted-Math-Control-Char.docx",
                   "oxpt/RP/RP001-Tracked-Revisions-01.docx", "oxpt/RP/RP001-Tracked-Revisions-01-Accepted.docx"}) {
        auto d = Document::open(std::string(DOCX_CORPUS) + "/" + f);
        std::string why;
        CHECK_EQ(schema_issues(d, &why), 0);
        if (!why.empty()) std::printf("    %s: %s\n", f, why.c_str());
    }
    // the math fix keeps the revision: rejecting the insertion still removes the "2"
    auto d = Document::open(std::string(DOCX_CORPUS) + "/oxpt/RP/RP014-Inserted-Math-Control-Char.docx");
    CHECK_EQ((int)list_revisions(d).size(), 1);
    reject_revisions(d, {});
    CHECK(analyze(paragraphs(d.body())[0]).utf8().find("A=2") == std::string::npos);
}

// Control characters and broken UTF-8 never reach the saved XML.
TEST(invalid_xml_chars_dropped) {
    auto d = Document::create();
    set_core_properties(d, Json{{"title", std::string("t\x01i\x1ftle\xff")}});
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"a\u0002b", nullptr, {});
    auto b = d.to_bytes();
    auto re = Document::from_bytes(b);
    std::string why;
    CHECK_EQ(schema_issues(re, &why), 0);
    auto& core = re.pkg().xml(re.pkg().related("/", opc::rt::coreProps));
    std::string title = core.document_element().child("dc:title").text().get();
    CHECK_EQ(title, std::string("title\xEF\xBF\xBD"));
}

TEST(lengths) {
    CHECK_EQ(util::length_twips(Json(12)), 240L);
    CHECK_EQ(util::length_twips(Json("1in")), 1440L);
    CHECK_EQ(util::length_twips(Json("1 in")), 1440L);
    CHECK_EQ(util::length_twips(Json("2.54cm")), 1440L);
    CHECK_EQ(util::length_twips(Json("240tw")), 240L);
    CHECK_EQ(util::length_twips(Json("6")), 120L);
    for (auto bad : {"1 parsec", "inf", "nan", "0x10", "", "1e308", "abc"}) {
        bool threw = false;
        try { util::length_twips(Json(bad)); } catch (std::exception&) { threw = true; }
        if (!threw) std::printf("    accepted: %s\n", bad);
        CHECK(threw);
    }
    CHECK_EQ(util::to_int("12abc"), 12);
    CHECK_EQ(util::to_int("junk", 7), 7);
    CHECK_EQ(util::to_int("99999999999999"), 2147483647);
}

// zip entries nobody gave a content type survive a save
TEST(untyped_parts_kept) {
    auto d = Document::create();
    d.pkg().add_part("/custom/blob.bin", "", util::Bytes{1, 2, 3});
    auto re = Document::from_bytes(d.to_bytes());
    CHECK(re.pkg().has("/custom/blob.bin"));
    CHECK(re.pkg().has("custom/blob.bin"));
}

int main(int argc, char** argv) { return t::run(argc, argv); }

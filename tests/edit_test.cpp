// Editing tests. Each edit is checked with three oracles:
//  - our schema validator (same results as the Open XML SDK on the corpus)
//  - accept-all gives the intended text
//  - reject-all gives the original text
// Output files go to build/tests/out/ for the external oracle (oxval) and
// LibreOffice round-trips.
#include <filesystem>

#include "schema/schema.hpp"
#include "t.hpp"
#include "util/util.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

using namespace docx;
using namespace docx::wml;
namespace fs = std::filesystem;

namespace {

std::string outdir() {
    fs::create_directories("out");
    return "out/";
}

std::vector<std::string> texts(Document& d) {
    std::vector<std::string> v;
    for (auto p : paragraphs(d.body(), false)) v.push_back(analyze(p).utf8());
    return v;
}

std::string joined(Document& d) {
    std::string s;
    for (auto& t : texts(d)) s += t + "\n";
    return s;
}

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

// Save, reopen, validate; then check accept/reject views.
void verify(Document& d, const std::string& name, const std::string& original, const std::string& intended) {
    auto bytes = d.to_bytes();
    {
        std::FILE* f = std::fopen((outdir() + name + ".docx").c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    auto re = Document::from_bytes(bytes);
    std::string why;
    int n = schema_issues(re, &why);
    if (n) t::fail(name.c_str(), 0, "schema: " + why);
    {
        auto a = Document::from_bytes(bytes);
        accept_revisions(a, {});
        if (joined(a) != intended) t::fail(name.c_str(), 0, "accept view: " + joined(a) + " expected " + intended);
        std::string w2;
        if (schema_issues(a, &w2)) t::fail(name.c_str(), 0, "after accept: " + w2);
    }
    {
        auto r = Document::from_bytes(bytes);
        reject_revisions(r, {});
        if (joined(r) != original) t::fail(name.c_str(), 0, "reject view: " + joined(r) + " expected " + original);
        std::string w2;
        if (schema_issues(r, &w2)) t::fail(name.c_str(), 0, "after reject: " + w2);
    }
}

Document sample(bool track) {
    auto d = Document::create();
    d.author.name = "Test Author";
    d.author.date = "2024-05-01T10:00:00Z";
    auto p = paragraphs(d.body())[0];
    EditOpts o{false};
    insert_text(d, p, 0, U"The quick brown fox jumps over the lazy dog.", nullptr, o);
    insert_paragraph(d, p, true, U"Second paragraph here.", "", nullptr, o);
    d.set_track_changes(track);
    return d;
}

const std::string kOrig = "The quick brown fox jumps over the lazy dog.\nSecond paragraph here.\n";

}  // namespace

TEST(blank_document_is_valid) {
    auto d = Document::create();
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    verify(d, "blank", "\n", "\n");
}

TEST(untracked_insert) {
    auto d = sample(false);
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 4, U"very ", nullptr, {});
    CHECK_EQ(analyze(p).utf8(), std::string("The very quick brown fox jumps over the lazy dog."));
    verify(d, "untracked_insert", "The very quick brown fox jumps over the lazy dog.\nSecond paragraph here.\n",
           "The very quick brown fox jumps over the lazy dog.\nSecond paragraph here.\n");
}

TEST(tracked_insert) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 4, U"very ", nullptr, {});
    CHECK_EQ(analyze(p).utf8(), std::string("The very quick brown fox jumps over the lazy dog."));
    CHECK(xml::first_descendant(p, "w:ins"));
    CHECK_EQ(xml::attr(xml::first_descendant(p, "w:ins"), "w:author"), std::string("Test Author"));
    verify(d, "tracked_insert", kOrig, "The very quick brown fox jumps over the lazy dog.\nSecond paragraph here.\n");
}

TEST(tracked_delete) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    delete_text(d, p, 4, 10, {});  // "quick "
    CHECK_EQ(analyze(p).utf8(), std::string("The brown fox jumps over the lazy dog."));
    CHECK(xml::first_descendant(p, "w:delText"));
    verify(d, "tracked_delete", kOrig, "The brown fox jumps over the lazy dog.\nSecond paragraph here.\n");
}

TEST(tracked_replace) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    replace_text(d, p, 16, 19, U"cat", nullptr, {});
    CHECK_EQ(analyze(p).utf8(), std::string("The quick brown cat jumps over the lazy dog."));
    // deletion precedes insertion (Word order)
    auto del = xml::first_descendant(p, "w:del");
    auto ins = xml::first_descendant(p, "w:ins");
    CHECK(del && ins);
    if (del && ins) CHECK(del.next_sibling() == ins);
    verify(d, "tracked_replace", kOrig, "The quick brown cat jumps over the lazy dog.\nSecond paragraph here.\n");
}

TEST(delete_own_insertion_removes_it) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 4, U"very ", nullptr, {});
    delete_text(d, p, 4, 9, {});
    CHECK(!xml::first_descendant(p, "w:ins"));
    CHECK(!xml::first_descendant(p, "w:del"));
    verify(d, "delete_own_insertion", kOrig, kOrig);
}

TEST(delete_others_insertion_nests) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 4, U"very ", nullptr, {});
    d.author.name = "Reviewer Two";
    delete_text(d, p, 4, 9, {});
    auto ins = xml::first_descendant(p, "w:ins");
    CHECK(ins);
    if (ins) CHECK(xml::first_descendant(ins, "w:del"));
    verify(d, "delete_others_insertion", kOrig, kOrig);
}

TEST(tracked_format) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    format_runs(d, p, 4, 9, {{"bold", true}, {"color", "FF0000"}}, {});
    auto ch = xml::first_descendant(p, "w:rPrChange");
    CHECK(ch);
    verify(d, "tracked_format", kOrig, kOrig);
    // reject restores no bold
    auto r = Document::from_bytes(d.to_bytes());
    reject_revisions(r, {});
    CHECK(!xml::first_descendant(r.body(), "w:b"));
    auto a = Document::from_bytes(d.to_bytes());
    accept_revisions(a, {});
    CHECK(xml::first_descendant(a.body(), "w:b"));
    CHECK(!xml::first_descendant(a.body(), "w:rPrChange"));
}

TEST(tracked_paragraph_format) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    format_paragraph(d, p, {{"alignment", "center"}, {"space_after", 12}}, {});
    CHECK(xml::first_descendant(p, "w:pPrChange"));
    verify(d, "tracked_paragraph_format", kOrig, kOrig);
    auto r = Document::from_bytes(d.to_bytes());
    reject_revisions(r, {});
    CHECK(!xml::first_descendant(r.body(), "w:jc"));
}

TEST(tracked_split_paragraph) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    split_paragraph(d, p, 20, {});
    auto ps = texts(d);
    CHECK_EQ(ps.size(), std::size_t(3));
    verify(d, "tracked_split", kOrig, "The quick brown fox \njumps over the lazy dog.\nSecond paragraph here.\n");
}

TEST(tracked_insert_paragraph) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    insert_paragraph(d, p, true, U"Inserted line.", "", nullptr, {});
    verify(d, "tracked_insert_paragraph", kOrig,
           "The quick brown fox jumps over the lazy dog.\nInserted line.\nSecond paragraph here.\n");
}

TEST(tracked_delete_paragraph) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    delete_paragraph(d, p, {});
    verify(d, "tracked_delete_paragraph", kOrig, "Second paragraph here.\n");
}

TEST(tracked_merge_paragraphs) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    merge_with_next(d, p, {});
    verify(d, "tracked_merge", kOrig, "The quick brown fox jumps over the lazy dog.Second paragraph here.\n");
}

TEST(find_replace_tracked) {
    auto d = sample(true);
    FindOpts f;
    int n = replace_all(d, "e", "E", f, nullptr, {});
    CHECK(n > 3);
    verify(d, "find_replace", kOrig, "ThE quick brown fox jumps ovEr thE lazy dog.\nSEcond paragraph hErE.\n");
}

TEST(find_replace_regex) {
    auto d = sample(false);
    FindOpts f;
    f.regex = true;
    int n = replace_all(d, "(\\w+) (\\w+) fox", "$2 $1 fox", f, nullptr, {});
    CHECK_EQ(n, 1);
    CHECK_EQ(texts(d)[0], std::string("The brown quick fox jumps over the lazy dog."));
}

TEST(insert_multiline_paragraphs) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[1];
    insert_paragraph(d, p, true, U"Line A\nLine B", "", nullptr, {});
    verify(d, "insert_multiline", kOrig, "The quick brown fox jumps over the lazy dog.\nSecond paragraph here.\nLine A\nLine B\n");
}

TEST(accept_by_author) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"A ", nullptr, {});
    d.author.name = "B";
    insert_text(d, p, 0, U"B ", nullptr, {});
    RevFilter f;
    f.author = "B";
    CHECK(accept_revisions(d, f) >= 1);
    auto revs = list_revisions(d);
    bool only_a = !revs.empty();
    for (auto& r : revs) if (r.author != "Test Author") only_a = false;
    CHECK(only_a);
}

// a copy of a paragraph with a hyperlink, a comment and old revisions becomes
// one clean insertion (w:ins inside w:hyperlink, never around it)
TEST(mark_paragraph_inserted_copy) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    delete_text(d, p, 0, 4, {});  // an old deletion
    pugi::xml_document frag;
    xml::parse_fragment("<w:hyperlink w:anchor=\"x\"><w:r><w:t>link</w:t></w:r></w:hyperlink>"
                        "<w:commentRangeStart w:id=\"7\"/><w:r><w:t>c</w:t></w:r><w:commentRangeEnd w:id=\"7\"/>"
                        "<w:r><w:commentReference w:id=\"7\"/></w:r>", frag);
    for (auto c : frag.children()) p.append_copy(c);
    auto np = p.parent().insert_copy_after(p, p);
    xml::set_attr(np, "w14:paraId", d.new_hex_id());
    mark_paragraph_inserted(d, np);
    CHECK(!xml::first_descendant(np, "w:del"));
    CHECK(!xml::first_descendant(np, "w:commentRangeStart"));
    CHECK(!xml::first_descendant(np, "w:commentReference"));
    CHECK(xml::first_descendant(np.child("w:hyperlink"), "w:ins"));
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    reject_revisions(d, {});
    CHECK_EQ((int)paragraphs(d.body()).size(), 2);
}

// rejecting everything a tracked session added to an empty document keeps
// the paragraph Word requires (was an empty body, then a crash)
TEST(reject_keeps_required_paragraphs) {
    // a body whose only block is a tracked-inserted table: rejecting removes
    // the table, and the body must still end with a paragraph
    auto d = Document::from_bytes(Document::create().to_bytes());
    auto body = d.body();
    pugi::xml_document frag;
    xml::parse_fragment("<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"100\"/>"
                        "</w:tblGrid><w:tr><w:trPr><w:ins w:id=\"1\" w:author=\"A\"/></w:trPr><w:tc><w:p/></w:tc></w:tr></w:tbl>",
                        frag);
    for (auto p : paragraphs(body)) body.remove_child(p);
    body.insert_copy_before(frag.first_child(), body.child("w:sectPr"));
    reject_revisions(d, {});
    CHECK(!body.child("w:tbl"));
    CHECK_EQ((int)paragraphs(d.body()).size(), 1);
    CHECK_EQ(schema_issues(d), 0);
}

// accepting one revision must leave the others' recorded old properties intact
TEST(partial_accept_keeps_prop_changes) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    format_paragraph(d, p, Json{{"style", "Heading 2"}}, {});
    insert_text(d, p, 1, U"x", nullptr, {});
    std::string ins_id;
    for (auto& r : list_revisions(d))
        if (r.type == "insertion") ins_id = r.id;
    RevFilter f;
    f.ids = {ins_id};
    accept_revisions(d, f);
    CHECK(p.child("w:pPr").child("w:pPrChange").child("w:pPr"));
    CHECK_EQ(schema_issues(d), 0);
}

// a tracked move is moveFrom/moveTo with named ranges; accept puts the text
// in its new place, reject leaves the document as it was
TEST(tracked_move_paragraphs) {
    auto build = []() {
        auto d = Document::from_bytes(Document::create().to_bytes());
        auto p0 = paragraphs(d.body())[0];
        std::vector<pugi::xml_node> ps;
        pugi::xml_node at = p0;
        for (auto t : {U"One", U"Two", U"Three", U"Four"}) ps.push_back(at = insert_paragraph(d, at, true, t, "", nullptr, {false}));
        return std::make_pair(std::move(d), ps);
    };
    auto texts = [](Document& d) {
        std::string s;
        for (auto p : paragraphs(d.body()))
            if (auto t = analyze(p).utf8(); !t.empty()) s += t + "|";
        return s;
    };
    for (bool after : {true, false}) {
        auto [d, ps] = build();
        CHECK_EQ(move_paragraphs(d, {ps[1], ps[2]}, after ? ps[3] : ps[0], after, {true}), 2);
        CHECK(xml::first_descendant(d.body(), "w:moveFromRangeStart"));
        CHECK(xml::first_descendant(d.body(), "w:moveToRangeStart"));
        CHECK_EQ(xml::attr(xml::first_descendant(d.body(), "w:moveFromRangeStart"), "w:name"),
                 xml::attr(xml::first_descendant(d.body(), "w:moveToRangeStart"), "w:name"));
        CHECK_EQ(schema_issues(d), 0);
        auto b = d.to_bytes();
        auto a = Document::from_bytes(b);
        accept_revisions(a, {});
        CHECK_EQ(texts(a), std::string(after ? "One|Four|Two|Three|" : "Two|Three|One|Four|"));
        CHECK_EQ(schema_issues(a), 0);
        auto r = Document::from_bytes(b);
        reject_revisions(r, {});
        CHECK_EQ(texts(r), std::string("One|Two|Three|Four|"));
        CHECK_EQ((int)paragraphs(r.body()).size(), 5);  // the blank first paragraph + four
        CHECK_EQ(schema_issues(r), 0);
    }
}

// copies of comment anchors (from edits that copy a paragraph) are dropped
TEST(dedupe_comment_anchors) {
    auto d = sample(false);
    auto p = paragraphs(d.body())[0];
    pugi::xml_document frag;
    xml::parse_fragment("<w:commentRangeStart w:id=\"3\"/><w:r><w:t>x</w:t></w:r><w:commentRangeEnd w:id=\"3\"/>"
                        "<w:r><w:commentReference w:id=\"3\"/></w:r>", frag);
    for (auto c : frag.children()) p.append_copy(c);
    auto q = paragraphs(d.body())[1];
    for (auto c : frag.children()) q.append_copy(c);
    d.dedupe_ids();
    CHECK_EQ((int)xml::descendants(d.body(), "w:commentRangeStart").size(), 1);
    CHECK_EQ((int)xml::descendants(d.body(), "w:commentReference").size(), 1);
    CHECK(xml::first_descendant(p, "w:commentReference"));
}

TEST(sha_vectors) {
    auto h = [](util::HashAlg a, const char* s) { return util::hex(util::hash(a, (const std::uint8_t*)s, std::strlen(s))); };
    CHECK_EQ(h(util::HashAlg::SHA1, "abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(h(util::HashAlg::SHA256, "abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(h(util::HashAlg::SHA512, "abc"), std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    CHECK_EQ(h(util::HashAlg::SHA384, "abc"), std::string("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));
    // multi-block
    std::string m(1000, 'a');
    CHECK_EQ(h(util::HashAlg::SHA256, m.c_str()), std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

int main(int argc, char** argv) { return t::run(argc, argv); }

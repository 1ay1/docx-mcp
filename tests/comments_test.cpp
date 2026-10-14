// Comment + style tests.
#include <filesystem>

#include "schema/schema.hpp"
#include "t.hpp"
#include "wml/comments.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/styles.hpp"
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
Document sample() {
    auto d = Document::create();
    d.author.name = "Ann Author";
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"Alpha beta gamma delta.", nullptr, {false});
    insert_paragraph(d, p, true, U"Second line.", "", nullptr, {false});
    return d;
}
}  // namespace

TEST(comment_basic) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    auto id = add_comment(d, p, 6, 10, "Check this word.");
    auto cs = list_comments(d);
    CHECK_EQ(cs.size(), std::size_t(1));
    CHECK_EQ(cs[0].anchor_text, std::string("beta"));
    CHECK_EQ(cs[0].text, std::string("Check this word."));
    CHECK_EQ(cs[0].author, std::string("Ann Author"));
    CHECK_EQ(cs[0].initials, std::string("AA"));
    CHECK(!cs[0].done);
    // text is unchanged by anchoring
    CHECK_EQ(analyze(p).utf8(), std::string("Alpha beta gamma delta."));
    CHECK(d.comments_ex(false));
    CHECK(d.comments_ids(false));
    CHECK(d.comments_extensible(false));
    CHECK(d.people(false));
    save(d, "comment_basic");
}

TEST(comment_reply_resolve) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    auto id = add_comment(d, p, 0, 5, "Root");
    d.author.name = "Bob Reviewer";
    auto r = reply_comment(d, id, "Reply one");
    auto r2 = reply_comment(d, r, "Reply two");  // reply to a reply threads to the root
    resolve_comment(d, id);
    auto cs = list_comments(d);
    CHECK_EQ(cs.size(), std::size_t(3));
    for (auto& c : cs) {
        if (c.id == r || c.id == r2) CHECK_EQ(c.parent_id, id);
        if (c.id == id) CHECK(c.done);
        CHECK_EQ(c.anchor_text, std::string("Alpha"));
    }
    save(d, "comment_thread");
    delete_comment(d, id);
    CHECK_EQ(list_comments(d).size(), std::size_t(0));
    CHECK(!xml::first_descendant(d.body(), "w:commentRangeStart"));
    CHECK(!xml::first_descendant(d.body(), "w:commentReference"));
    save(d, "comment_deleted");
}

TEST(comment_across_paragraphs) {
    auto d = sample();
    auto ps = paragraphs(d.body());
    add_comment_range(d, ps[0], 6, ps[1], 6, "Spans two paragraphs");
    auto cs = list_comments(d);
    CHECK_EQ(cs[0].anchor_text, std::string("beta gamma delta.\nSecond"));
    save(d, "comment_range");
}

TEST(comment_multiline_edit) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    auto id = add_comment(d, p, 0, 5, "line one\nline two");
    CHECK_EQ(list_comments(d)[0].text, std::string("line one\nline two"));
    auto pid = list_comments(d)[0].para_id;
    edit_comment(d, id, "changed");
    CHECK_EQ(list_comments(d)[0].text, std::string("changed"));
    CHECK_EQ(list_comments(d)[0].para_id, pid);  // identity stable for threading
    save(d, "comment_edit");
}

TEST(comment_on_corpus_document) {
    // existing Word comments survive and new ones thread correctly
    auto d = Document::open(DOCX_CORPUS "/oxpt/HC031-Complicated-Document.docx");
    auto before = list_comments(d);
    CHECK(!before.empty());
    auto r = reply_comment(d, before[0].id, "Agreed.");
    auto after = list_comments(d);
    CHECK_EQ(after.size(), before.size() + 1);
    bool ok = false;
    for (auto& c : after) if (c.id == r) ok = c.parent_id == before[0].id;
    CHECK(ok);
    save(d, "comment_corpus_reply");
}

TEST(builtin_styles_on_demand) {
    auto d = sample();
    CHECK(!find_style(d, "Heading1"));
    auto id = ensure_style(d, "heading 1");
    CHECK_EQ(id, std::string("Heading1"));
    CHECK(find_style(d, "Heading1Char"));  // linked char style comes along
    format_paragraph(d, paragraphs(d.body())[0], {{"style", "Heading1"}}, {false});
    auto eff = effective_paragraph(d, paragraphs(d.body())[0]);
    CHECK(eff.value("keep_with_next", false));
    save(d, "style_heading");
}

TEST(custom_style) {
    auto d = sample();
    auto id = upsert_style(d, "", {{"name", "My Quote"}, {"based_on", "Normal"},
                                   {"paragraph", {{"indent_left", "0.5in"}, {"alignment", "center"}}},
                                   {"run", {{"italic", true}, {"color", "595959"}}}});
    CHECK_EQ(id, std::string("MyQuote"));
    format_paragraph(d, paragraphs(d.body())[1], {{"style", id}}, {false});
    auto eff = effective_paragraph(d, paragraphs(d.body())[1]);
    CHECK_EQ(eff.value("alignment", std::string()), std::string("center"));
    auto p = paragraphs(d.body())[1];
    auto er = effective_run(d, p, xml::first_descendant(p, "w:r"));
    CHECK(er.value("italic", false));
    save(d, "style_custom");
    delete_style(d, id);
    CHECK(!find_style(d, id));
    CHECK(!xml::first_descendant(d.body(), "w:pStyle"));
}

TEST(effective_defaults) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    auto er = effective_run(d, p, xml::first_descendant(p, "w:r"));
    CHECK_EQ(er.value("font", std::string()), std::string("Calibri"));
    CHECK_EQ(er.value("size", 0.0), 11.0);
    auto ep = effective_paragraph(d, p);
    CHECK_EQ(ep.value("space_after", 0.0), 8.0);
}

int main(int argc, char** argv) { return t::run(argc, argv); }

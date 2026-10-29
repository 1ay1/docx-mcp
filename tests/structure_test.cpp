// Structure tests: tables, pictures, sections, headers/footers, notes,
// bookmarks, hyperlinks, fields, TOC, content controls, lists, equations.
#include <filesystem>

#include "schema/schema.hpp"
#include "t.hpp"
#include "util/util.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/structure.hpp"
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
    // accept/reject must stay valid too
    for (bool acc : {true, false}) {
        auto x = Document::from_bytes(b);
        if (acc) accept_revisions(x, {}); else reject_revisions(x, {});
        std::string w2;
        if (schema_issues(x, &w2)) t::fail(name.c_str(), 0, std::string(acc ? "after accept: " : "after reject: ") + w2);
    }
}
Document sample(bool track = false) {
    auto d = Document::create();
    d.author.name = "Tess Tables";
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"Intro paragraph.", nullptr, {false});
    insert_paragraph(d, p, true, U"Closing paragraph.", "", nullptr, {false});
    d.set_track_changes(track);
    return d;
}
// 1x1 red PNG
util::Bytes png() {
    auto b = util::base64_decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==");
    return *b;
}
}  // namespace

TEST(table_insert_and_edit) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    auto tbl = insert_table(d, p, true, {{"Name", "Qty"}, {"Apples", "3"}, {"Pears", "5"}}, {{"style", "TableGrid"}}, {});
    CHECK_EQ(table_json(tbl)["rows"][1][0].get<std::string>(), std::string("Apples"));
    insert_row(d, tbl, 2, true, {"Plums", "7"}, {});
    insert_column(d, tbl, 1, true, {});
    set_cell_text(d, cell(tbl, 0, 2), "Price", {});
    format_cell(d, cell(tbl, 0, 0), {{"shading", "D9E2F3"}}, {});
    format_table(d, tbl, {{"alignment", "center"}, {"repeat_header", true}}, {});
    auto j = table_json(tbl);
    CHECK_EQ(j["rows"].size(), std::size_t(4));
    CHECK_EQ(j["rows"][0].size(), std::size_t(3));
    CHECK_EQ(j["rows"][3][0].get<std::string>(), std::string("Plums"));
    merge_cells(d, tbl, 1, 0, 2, 0);
    delete_row(d, tbl, 3, {});
    CHECK_EQ(table_json(tbl)["rows"].size(), std::size_t(3));
    save(d, "table_edit");
}

TEST(table_tracked) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    auto tbl = insert_table(d, p, true, {{"a", "b"}, {"c", "d"}}, {}, {});
    CHECK(xml::first_descendant(tbl, "w:trPr").child("w:ins"));
    save(d, "table_tracked_insert");
    // reject removes the table entirely
    auto r = Document::from_bytes(d.to_bytes());
    reject_revisions(r, {});
    CHECK(tables(r).empty());
    auto d2 = sample(false);
    auto t2 = insert_table(d2, paragraphs(d2.body())[0], true, {{"a", "b"}, {"c", "d"}}, {}, {});
    d2.set_track_changes(true);
    delete_row(d2, t2, 1, {});
    insert_row(d2, t2, 0, true, {"x", "y"}, {});
    save(d2, "table_tracked_rows");
    auto a = Document::from_bytes(d2.to_bytes());
    accept_revisions(a, {});
    auto ja = table_json(tables(a)[0]);
    CHECK_EQ(ja["rows"].size(), std::size_t(2));
    CHECK_EQ(ja["rows"][1][0].get<std::string>(), std::string("x"));
}

// every Layout Option makes a valid anchor, can be changed and turned back
// inline; delete works for floating ones too
TEST(picture_floating) {
    auto d = sample(false);
    auto p = paragraphs(d.body())[0];
    auto png = util::base64_decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
    for (auto w : {"square", "tight", "through", "top_bottom", "behind", "in_front"})
        insert_picture(d, p, 0, *png, 20, 0, "pic", "", {}, placement_from_json(Json(w)));
    auto ps = list_pictures(d);
    CHECK_EQ((int)ps.size(), 6);
    CHECK_EQ((int)xml::descendants(d.body(), "wp:anchor").size(), 6);
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    set_picture(d, 1, 40, 0, Json{{"wrap", "square"}, {"horizontal", "center"}, {"vertical_from", "page"}, {"y", "1in"}},
                Json("moved"));
    auto first = list_pictures(d)[0];
    CHECK_EQ(first.alt, std::string("moved"));
    CHECK(first.width_pt > 39.9 && first.width_pt < 40.1);
    set_picture(d, 2, 0, 0, Json("inline"), Json());
    CHECK_EQ((int)xml::descendants(d.body(), "wp:inline").size(), 1);
    delete_picture(d, 3, {});
    CHECK_EQ((int)list_pictures(d).size(), 5);
    bool threw = false;
    try { placement_from_json(Json{{"wrap", "square"}, {"vertical_from", "sky"}}); } catch (std::exception&) { threw = true; }
    CHECK(threw);
    CHECK_EQ(schema_issues(d), 0);
    save(d, "picture_floating");
}

TEST(picture_inline) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    insert_picture(d, p, 5, png(), 72, 0, "A red dot", "", {});
    auto pt = analyze(p);
    CHECK_EQ(pt.text.size(), std::size_t(17));
    CHECK(xml::first_descendant(p, "pic:pic"));
    bool media = false;
    for (auto& n : d.pkg().part_names()) if (n.find("/word/media/") == 0) media = true;
    CHECK(media);
    save(d, "picture");
}

TEST(sections_headers_footers) {
    auto d = sample();
    auto ps = paragraphs(d.body());
    insert_section_break(d, ps[0], "nextPage", {});
    CHECK_EQ(sections(d).size(), std::size_t(2));
    format_section(d, 2, {{"orientation", "landscape"}, {"margins", {{"top", "0.5in"}}}}, {});
    auto sj = section_json(d, sections(d)[1]);
    CHECK_EQ(sj["orientation"].get<std::string>(), std::string("landscape"));
    set_header_footer_text(d, 1, "header", "default", "Company Confidential", {{"alignment", "center"}});
    set_header_footer_text(d, 1, "footer", "default", "Page {PAGE} of {NUMPAGES}", {{"alignment", "right"}});
    set_header_footer_text(d, 2, "header", "first", "First page header", {});
    auto hp = header_footer(d, 1, "footer", "default", false);
    CHECK(!hp.empty());
    std::string fx = xml::to_string(d.pkg().xml(hp).document_element());
    CHECK(fx.find("PAGE") != std::string::npos);
    save(d, "sections_hf");
}

TEST(notes_bookmarks_links) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    add_note(d, p, 5, "footnote", "A footnote.", {});
    add_note(d, p, 16, "endnote", "An endnote.", {});
    auto ns = list_notes(d);
    CHECK_EQ(ns.size(), std::size_t(2));
    CHECK_EQ(ns[0].text, std::string("A footnote."));
    d.set_track_changes(false);
    auto p2 = paragraphs(d.body())[1];
    add_bookmark(d, p2, 0, p2, 7, "closing");
    auto bs = list_bookmarks(d);
    bool found = false;
    for (auto& b : bs) if (b.name == "closing") { found = true; CHECK_EQ(b.text, std::string("Closing")); }
    CHECK(found);
    add_hyperlink(d, p2, 8, 17, "https://example.com/", "", "Example", {});
    add_hyperlink(d, p2, 0, 0, "#closing", "jump ", "", {});
    auto ls = list_hyperlinks(d);
    CHECK_EQ(ls.size(), std::size_t(2));
    save(d, "notes_links");
}

TEST(fields_and_toc) {
    auto d = sample();
    auto ps = paragraphs(d.body());
    auto h1 = insert_paragraph(d, ps[0], true, U"Chapter One", "Heading1", nullptr, {});
    ensure_style(d, "Heading1");
    auto h2 = insert_paragraph(d, h1, true, U"Section A", "Heading2", nullptr, {});
    ensure_style(d, "Heading2");
    (void)h2;
    insert_toc(d, paragraphs(d.body())[0], {{"to_level", 3}}, {});
    auto fs = list_fields(d);
    bool toc = false;
    for (auto& f : fs) if (util::starts_with(f.instr, "TOC")) toc = true;
    CHECK(toc);
    auto cap = insert_paragraph(d, paragraphs(d.body()).back(), true, U"Figure ", "Caption", nullptr, {});
    ensure_style(d, "Caption");
    insert_field(d, cap, 7, "SEQ Figure \\* ARABIC", "9", nullptr, {});
    auto cap2 = insert_paragraph(d, cap, true, U"Figure ", "Caption", nullptr, {});
    insert_field(d, cap2, 7, "SEQ Figure \\* ARABIC", "9", nullptr, {});
    update_fields(d);
    CHECK_EQ(analyze(cap).utf8(), std::string("Figure 1"));
    CHECK_EQ(analyze(cap2).utf8(), std::string("Figure 2"));
    CHECK(d.settings().child("w:updateFields"));
    save(d, "fields_toc");
}

TEST(content_controls) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    add_content_control(d, p, 0, 5, {{"type", "text"}, {"tag", "intro"}, {"alias", "Intro"}});
    auto p2 = paragraphs(d.body())[1];
    add_content_control(d, p2, (int)analyze(p2).vis.size(), (int)analyze(p2).vis.size(), {{"type", "checkbox"}, {"tag", "agree"}});
    add_content_control(d, p2, 0, 0, {{"type", "dropDown"}, {"tag", "pick"}, {"items", {"Red", "Green"}}});
    set_content_control(d, "intro", "Hello");
    set_content_control(d, "agree", true);
    set_content_control(d, "pick", "Green");
    auto cs = list_content_controls(d);
    CHECK_EQ(cs.size(), std::size_t(3));
    for (auto& c : cs) {
        if (c.tag == "intro") CHECK_EQ(c.text, std::string("Hello"));
        if (c.tag == "agree") CHECK(c.checked);
        if (c.tag == "pick") CHECK_EQ(c.text, std::string("Green"));
    }
    save(d, "content_controls");
}

TEST(lists) {
    auto d = sample();
    auto p = paragraphs(d.body())[0];
    auto a = insert_paragraph(d, p, true, U"one\ntwo\nthree", "", nullptr, {});
    std::vector<pugi::xml_node> items;
    for (auto n = a; items.size() < 3; n = n.next_sibling("w:p")) items.push_back(n);
    apply_list(d, items, "number", 0, {}, {});
    CHECK_EQ(list_label(d, items[0]), std::string("1."));
    CHECK_EQ(list_label(d, items[2]), std::string("3."));
    format_paragraph(d, items[1], {{"numbering", {{"num_id", std::stoi(xml::attr(items[0].child("w:pPr").child("w:numPr").child("w:numId"), "w:val"))}, {"level", 1}}}}, {});
    CHECK_EQ(list_label(d, items[1]), std::string("a."));
    CHECK_EQ(list_label(d, items[2]), std::string("2."));
    restart_numbering(d, items[2], 5);
    CHECK_EQ(list_label(d, items[2]), std::string("5."));
    auto b = insert_paragraph(d, paragraphs(d.body()).back(), true, U"dot", "", nullptr, {});
    apply_list(d, {b}, "bullet", 0, {}, {});
    CHECK_EQ(list_label(d, b), std::string("\u2022"));
    save(d, "lists");
    remove_list(d, {b}, {});
    CHECK_EQ(list_label(d, b), std::string(""));
}

TEST(multilevel_lists) {
    auto d = Document::create();
    auto p0 = paragraphs(d.body())[0];
    // outline preset: I. / A. / 1.
    int nid = define_list(d, {{"preset", "outline"}});
    std::vector<pugi::xml_node> ps;
    auto last = p0;
    int lv[] = {0, 1, 2, 2, 1, 0, 1};
    for (int k = 0; k < 7; k++) {
        last = insert_paragraph(d, last, true, U"item", "", nullptr, {});
        format_paragraph(d, last, {{"numbering", {{"num_id", nid}, {"level", lv[k]}}}}, {});
        ps.push_back(last);
    }
    CHECK_EQ(list_label(d, ps[0]), std::string("I."));
    CHECK_EQ(list_label(d, ps[1]), std::string("A."));
    CHECK_EQ(list_label(d, ps[2]), std::string("1."));
    CHECK_EQ(list_label(d, ps[3]), std::string("2."));
    CHECK_EQ(list_label(d, ps[4]), std::string("B."));
    CHECK_EQ(list_label(d, ps[5]), std::string("II."));
    CHECK_EQ(list_label(d, ps[6]), std::string("A."));
    // legal-style numbers with an override: level 2 never restarts
    int n2 = define_list(d, {{"preset", "numbered"}, {"levels", Json::array({Json::object(), {{"text", "%1.%2)"}, {"restart_after", false}}})}});
    std::vector<pugi::xml_node> qs;
    int lv2[] = {0, 1, 1, 0, 1};
    for (int k = 0; k < 5; k++) {
        last = insert_paragraph(d, last, true, U"x", "", nullptr, {});
        format_paragraph(d, last, {{"numbering", {{"num_id", n2}, {"level", lv2[k]}}}}, {});
        qs.push_back(last);
    }
    CHECK_EQ(list_label(d, qs[1]), std::string("1.1)"));
    CHECK_EQ(list_label(d, qs[2]), std::string("1.2)"));
    CHECK_EQ(list_label(d, qs[4]), std::string("2.3)"));
    // numbered headings through linked styles
    define_list(d, {{"preset", "headings"}});
    auto h1 = insert_paragraph(d, last, true, U"Intro", "Heading1", nullptr, {});
    auto h2 = insert_paragraph(d, h1, true, U"Scope", "Heading2", nullptr, {});
    auto h2b = insert_paragraph(d, h2, true, U"Terms", "Heading2", nullptr, {});
    auto h1b = insert_paragraph(d, h2b, true, U"Design", "Heading1", nullptr, {});
    CHECK_EQ(list_label(d, h1), std::string("1"));
    CHECK_EQ(list_label(d, h2b), std::string("1.2"));
    CHECK_EQ(list_label(d, h1b), std::string("2"));
    auto defs = list_definitions(d);
    CHECK_EQ(defs.size(), std::size_t(3));
    bool bad = false;
    try { define_list(d, {{"levels", Json::array({{{"text", "%2."}}})}}); } catch (std::exception&) { bad = true; }
    CHECK(bad);
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    save(d, "multilevel_lists");
}

TEST(table_style_conditional) {
    auto d = Document::create();
    auto id = upsert_style(d, "", {{"name", "Report Table"}, {"type", "table"},
                                    {"table", {{"borders", {{"style", "single"}, {"size", 0.5}, {"color", "9BC2E6"}}}, {"band_size", 1}}},
                                    {"conditional", {{"header_row", {{"run", {{"bold", true}, {"color", "FFFFFF"}}}, {"cell", {{"shading", "#1F4E79"}}}}},
                                                     {"banded_rows", {{"cell", {{"shading", "DDEBF7"}}}}},
                                                     {"total_row", {{"bold", true}, {"borders", {{"top", {{"style", "double"}}}}}}},
                                                     {"first_column", {{"run", {{"italic", true}}}}}}}});
    CHECK_EQ(id, std::string("ReportTable"));
    auto st = find_style(d, id);
    int parts = 0;
    std::vector<std::string> order;
    for (auto p : st.children("w:tblStylePr")) { parts++; order.push_back(xml::attr(p, "w:type")); }
    CHECK_EQ(parts, 4);
    if (parts == 4) {
        CHECK_EQ(order[0], std::string("firstRow"));
        CHECK_EQ(order[1], std::string("lastRow"));
        CHECK_EQ(order[2], std::string("firstCol"));
        CHECK_EQ(order[3], std::string("band1Horz"));
    }
    auto j = table_style_json(st);
    CHECK_EQ(j["conditional"]["header_row"]["cell"]["shading"].get<std::string>(), std::string("1F4E79"));
    CHECK(j["conditional"]["header_row"]["run"]["bold"].get<bool>());
    // remove a part, use the style on a table
    upsert_style(d, id, {{"conditional", {{"first_column", nullptr}}}});
    CHECK(table_style_json(find_style(d, id))["conditional"].size() == 3);
    auto tbl = insert_table(d, paragraphs(d.body())[0], true, {{"Item", "Cost"}, {"A", "1"}, {"B", "2"}, {"Total", "3"}},
                            {{"style", id}, {"header_row", true}}, {});
    format_table(d, tbl, {{"total_row", true}, {"banded_rows", true}}, {});
    bool bad = false;
    try { upsert_style(d, id, {{"conditional", {{"header", {{"bold", true}}}}}}); } catch (std::exception&) { bad = true; }
    CHECK(bad);
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    save(d, "table_style_conditional");
}

TEST(import_styles_from_template) {
    auto src = Document::create();
    upsert_style(src, "", {{"name", "Brand Base"}, {"type", "paragraph"}, {"run", {{"font", "Georgia"}, {"color", "1F4E79"}}}});
    upsert_style(src, "", {{"name", "Brand Title"}, {"type", "paragraph"}, {"based_on", "BrandBase"}, {"run", {{"size", 28}}}});
    int nid = define_list(src, {{"preset", "outline"}});
    upsert_style(src, "", {{"name", "Brand List"}, {"type", "paragraph"}, {"paragraph", {{"numbering", {{"num_id", nid}, {"level", 0}}}}}});
    auto d = Document::create();
    upsert_style(d, "", {{"name", "Brand Title"}, {"type", "paragraph"}, {"run", {{"size", 10}}}});
    auto r = import_styles(d, src, {"Brand Title", "BrandList"}, false, false);
    CHECK(find_style(d, "BrandBase"));
    // Brand Title and Normal (its next/based-on chain) were already there
    CHECK_EQ(r["kept_existing"].size(), std::size_t(2));
    r = import_styles(d, src, {"Brand Title"}, true, true);
    CHECK_EQ(r["replaced"].size(), std::size_t(3));
    auto t = find_style(d, "BrandTitle");
    CHECK_EQ(xml::attr(t.child("w:basedOn"), "w:val"), std::string("BrandBase"));
    CHECK_EQ(xml::attr(t.child("w:rPr").child("w:sz"), "w:val"), std::string("56"));
    auto p = insert_paragraph(d, paragraphs(d.body())[0], true, U"one", "BrandList", nullptr, {});
    CHECK_EQ(list_label(d, p), std::string("I."));
    bool bad = false;
    try { import_styles(d, src, {"Nope"}, true, false); } catch (std::exception&) { bad = true; }
    CHECK(bad);
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    save(d, "import_styles");
}

TEST(lists_tracked) {
    auto d = sample(true);
    auto p = paragraphs(d.body())[0];
    apply_list(d, {p}, "bullet", 0, {}, {});
    CHECK(xml::first_descendant(p, "w:numPr").child("w:ins"));
    save(d, "lists_tracked");
    auto r = Document::from_bytes(d.to_bytes());
    reject_revisions(r, {});
    CHECK(!xml::first_descendant(r.body(), "w:numPr"));
}

TEST(equation) {
    auto d = sample();
    auto p = paragraphs(d.body())[1];
    insert_equation(d, p, 0, "x^2+y_1=\\frac{a}{b}", false, {});
    auto q = insert_paragraph(d, p, true, U"", "", nullptr, {});
    insert_equation(d, q, 0, "\\sum_{i=1}^{n} i = \\frac{n(n+1)}{2}", true, {});
    CHECK(xml::first_descendant(p, "m:f"));
    CHECK(xml::first_descendant(q, "m:nary"));
    save(d, "equation");
}

// non-ASCII letters and stray closers used to loop forever
TEST(equation_never_stalls) {
    auto d = sample();
    auto p = paragraphs(d.body())[1];
    for (const char* e : {"T = \\frac{\u0127 c^3}{8\\pi G M k_B}", "\u0127", "\u65e5\u672c+1", "a)b}c", "\\", "((", "\\sqrt{x", "\\sum x"})
        insert_equation(d, p, 0, e, false, {});
    CHECK(xml::descendants(p, "m:oMath").size() == 8);
    save(d, "equation_odd");
}

// tracked column edits never leave cellIns and cellDel on one cell
TEST(tracked_column_insert_delete) {
    auto d = sample(false);
    auto t = insert_table(d, paragraphs(d.body())[0], true, {{"a", "b"}, {"c", "d"}}, Json::object(), {});
    d.set_track_changes(true);
    insert_column(d, t, 1, true, {});
    CHECK_EQ((int)xml::descendants(t, "w:cellIns").size(), 2);
    delete_column(d, t, 2, {});  // own insertion: just goes
    CHECK_EQ((int)xml::descendants(t, "w:cellIns").size(), 0);
    CHECK_EQ((int)xml::children(xml::children(t, "w:tr")[0], "w:tc").size(), 2);
    insert_column(d, t, 1, true, {});
    d.author.name = "Someone Else";
    delete_column(d, t, 2, {});  // another author's insertion: becomes a deletion
    for (auto tcpr : xml::descendants(t, "w:tcPr"))
        CHECK(!(tcpr.child("w:cellIns") && tcpr.child("w:cellDel")));
    CHECK_EQ((int)xml::descendants(t, "w:cellDel").size(), 2);
    save(d, "tracked_column_insert_delete");
}

// a row inserted by one author and deleted by another: accept and reject both resolve it
TEST(tracked_row_inserted_then_deleted) {
    auto d = sample(false);
    auto t = insert_table(d, paragraphs(d.body())[0], true, {{"a", "b"}, {"c", "d"}}, Json::object(), {});
    d.set_track_changes(true);
    d.author.name = "First";
    insert_row(d, t, 0, true, {"x", "y"}, {});
    d.author.name = "Second";
    delete_row(d, t, 1, {});
    auto tr = xml::children(t, "w:tr")[1];
    CHECK(tr.child("w:trPr").child("w:ins") && tr.child("w:trPr").child("w:del"));
    auto b = d.to_bytes();
    for (bool acc : {true, false}) {
        auto x = Document::from_bytes(b);
        if (acc) accept_revisions(x, {}); else reject_revisions(x, {});
        CHECK_EQ((int)list_revisions(x).size(), 0);
        CHECK_EQ((int)xml::children(tables(x)[0], "w:tr").size(), 2);
    }
    save(d, "tracked_row_inserted_then_deleted");
}

TEST(cell_format_text_and_margins) {
    auto d = sample(false);
    auto t = insert_table(d, paragraphs(d.body())[0], true, {{"a", "b"}}, Json::object(), {});
    format_cell(d, cell(t, 0, 0), Json{{"bold", true}, {"alignment", "center"}, {"shading", "EEEEEE"}}, {});
    CHECK(xml::first_descendant(cell(t, 0, 0), "w:b"));
    CHECK(xml::first_descendant(cell(t, 0, 0), "w:jc"));
    format_table(d, t, Json{{"cell_margins", 5}}, {});
    CHECK(t.child("w:tblPr").child("w:tblCellMar").child("w:top"));
    bool threw = false;
    try { format_table(d, t, Json{{"cell_margins", Json{{"middle", 3}}}}, {}); } catch (std::exception&) { threw = true; }
    CHECK(threw);
    save(d, "cell_format_text_and_margins");
}

// rejecting an inserted row that holds an inserted nested table must not
// touch the nested rows after they're freed (was a use-after-free)
TEST(reject_nested_inserted_rows) {
    auto d = sample(false);
    auto t = insert_table(d, paragraphs(d.body())[0], true, {{"a", "b"}}, Json::object(), {});
    d.set_track_changes(true);
    auto row = insert_row(d, t, 0, true, {"x", "y"}, {});
    auto inner_p = xml::children(xml::children(row, "w:tc")[0], "w:p")[0];
    insert_table(d, inner_p, true, {{"n1"}, {"n2"}}, Json::object(), {});
    for (bool acc : {false, true}) {
        auto x = Document::from_bytes(d.to_bytes());
        if (acc) accept_revisions(x, {}); else reject_revisions(x, {});
        CHECK_EQ((int)list_revisions(x).size(), 0);
        CHECK_EQ(schema_issues(x), 0);
        CHECK_EQ((int)xml::descendants(x.body(), "w:tbl").size(), acc ? 2 : 1);
    }
}

// a tracked TOC wraps runs inside its hyperlinks, never the hyperlinks
TEST(split_cells_and_nested_tables) {
    auto d = sample(false);
    auto t = insert_table(d, paragraphs(d.body())[0], true, {{"a", "b", "c"}, {"d", "e", "f"}, {"g", "h", "i"}},
                          Json::object(), {});
    // undo a 2x2 merge: back to 3x3 cells
    merge_cells(d, t, 0, 0, 1, 1);
    CHECK_EQ(split_cell(d, t, 1, 1, 2), 4);
    for (auto tr : t.children("w:tr")) CHECK_EQ((int)xml::children(tr, "w:tc").size(), 3);
    CHECK(!xml::first_descendant(t, "w:vMerge") && !xml::first_descendant(t, "w:gridSpan"));
    // a plain cell into 3: the grid grows, other rows span the new columns
    CHECK_EQ(split_cell(d, t, 2, 2, 3), 3);
    CHECK_EQ((int)xml::children(t.child("w:tblGrid"), "w:gridCol").size(), 5);
    CHECK_EQ((int)xml::children(xml::children(t, "w:tr")[0], "w:tc").size(), 3);
    CHECK_EQ((int)xml::children(xml::children(t, "w:tr")[2], "w:tc").size(), 5);
    // a nested table is numbered right after its outer table
    auto inner_p = xml::children(cell(t, 1, 1), "w:p")[0];
    auto inner = insert_table(d, inner_p, true, {{"n1", "n2"}}, Json::object(), {});
    insert_table(d, paragraphs(d.body()).back(), true, {{"z"}}, Json::object(), {});
    auto ts = tables(d);
    CHECK_EQ((int)ts.size(), 3);
    CHECK(ts[1] == inner);
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    save(d, "split_cells_and_nested_tables");
}

// a tracked TOC wraps runs inside its hyperlinks, never the hyperlinks
TEST(tracked_toc_valid) {
    auto d = sample(false);
    auto ps = paragraphs(d.body());
    format_paragraph(d, ps[1], Json{{"style", "Heading 1"}}, {});
    insert_toc(d, ps[0], Json::object(), {true});
    for (auto h : xml::descendants(d.body(), "w:hyperlink")) CHECK(std::string_view(h.parent().name()) != "w:ins");
    std::string why;
    CHECK_EQ(schema_issues(d, &why), 0);
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    reject_revisions(d, {});
    CHECK(!xml::first_descendant(d.body(), "w:hyperlink"));
    CHECK(!xml::first_descendant(d.body(), "w:sdt"));
    CHECK_EQ(schema_issues(d), 0);
}

// captions number in document order; cross-references show label+number,
// text, number, a note's number; text typed after a field lands after it
TEST(captions_and_cross_references) {
    auto d = sample(false);
    auto ps = paragraphs(d.body());
    auto c2 = insert_caption(d, ps[1], true, "Figure", "Later", "", {});
    auto c1 = insert_caption(d, ps[0], true, "Figure", "Earlier", "", {});
    CHECK_EQ(analyze(c1).utf8(), std::string("Figure 1: Earlier"));
    CHECK_EQ(analyze(c2).utf8(), std::string("Figure 2: Later"));
    auto q = insert_paragraph(d, ps.back(), true, U"See ", "", nullptr, {});
    insert_cross_reference(d, q, (int)analyze(q).vis.size(), para_id(c2), "label_number", true, {});
    insert_text(d, q, (int)analyze(q).vis.size(), U", ", nullptr, {});
    insert_cross_reference(d, q, (int)analyze(q).vis.size(), para_id(c2), "text", true, {});
    insert_text(d, q, (int)analyze(q).vis.size(), U", ", nullptr, {});
    insert_cross_reference(d, q, (int)analyze(q).vis.size(), para_id(c2), "number", true, {});
    insert_text(d, q, (int)analyze(q).vis.size(), U".", nullptr, {});
    CHECK_EQ(analyze(q).utf8(), std::string("See Figure 2, Later, 2."));
    // a new caption before the referenced one renumbers it, and the reference follows
    insert_caption(d, paragraphs(d.body())[0], false, "Figure", "First", "", {});
    update_fields(d);
    CHECK_EQ(analyze(c2).utf8(), std::string("Figure 3: Later"));
    CHECK_EQ(analyze(q).utf8(), std::string("See Figure 3, Later, 3."));
    bool threw = false;
    try { insert_caption(d, ps[0], true, "Two words", "x", "", {}); } catch (std::exception&) { threw = true; }
    CHECK(threw);
    CHECK_EQ(schema_issues(d), 0);
    save(d, "captions_and_cross_references");
}

int main(int argc, char** argv) { return t::run(argc, argv); }

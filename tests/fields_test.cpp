// Field engine tests: formats, IF/COMPARE/=, SEQ, REF, DOCPROPERTY,
// DOCVARIABLE, mail merge, unlink, and page numbers from a real layout.
#include <cmath>
#include <filesystem>

#include "schema/schema.hpp"
#include "t.hpp"
#include "util/util.hpp"
#include "wml/docops.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/render.hpp"
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
}

// A one-paragraph document holding a field; returns the paragraph's text after update.
std::string eval(const std::string& code, Document* keep = nullptr, const FieldUpdate& u = {}) {
    Document d = keep ? Document::from_bytes(keep->snapshot()) : Document::create();
    auto p = paragraphs(d.body()).back();
    p = insert_paragraph(d, p, true, U"", "", nullptr, {});
    insert_field(d, p, 0, code, "old", nullptr, {});
    evaluate_fields(d, u, nullptr);
    return analyze(p).utf8();
}

std::tm date(int y, int m, int dd, int h = 0, int mi = 0) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = dd;
    t.tm_hour = h;
    t.tm_min = mi;
    std::tm c = t;
    std::mktime(&c);
    t.tm_wday = c.tm_wday;
    return t;
}

}  // namespace

TEST(date_pictures) {
    auto t = date(2024, 3, 5, 14, 7);
    CHECK_EQ(format_date(t, "MMMM d, yyyy"), std::string("March 5, 2024"));
    CHECK_EQ(format_date(t, "dddd, d MMM yy"), std::string("Tuesday, 5 Mar 24"));
    CHECK_EQ(format_date(t, "MM/dd/yyyy"), std::string("03/05/2024"));
    CHECK_EQ(format_date(t, "h:mm am/pm"), std::string("2:07 pm"));
    CHECK_EQ(format_date(t, "HH:mm AM/PM"), std::string("14:07 PM"));
    CHECK_EQ(format_date(t, "'Week of' MMM d"), std::string("Week of Mar 5"));
}

TEST(numeric_pictures) {
    CHECK_EQ(format_numeric(1234.5, "#,##0.00"), std::string("1,234.50"));
    CHECK_EQ(format_numeric(1234.5, "$#,##0"), std::string("$1,235"));
    CHECK_EQ(format_numeric(-3, "0.0;(0.0)"), std::string("(3.0)"));
    CHECK_EQ(format_numeric(0, "0;-0;'zero'"), std::string("'zero'"));
    CHECK_EQ(format_numeric(0.5, "0%"), std::string("1%"));
    CHECK_EQ(format_numeric(7, "00"), std::string("07"));
    CHECK_EQ(format_numeric(-7, "0"), std::string("-7"));
    CHECK_EQ(format_numeric(3.14159, "0.##"), std::string("3.14"));
    CHECK_EQ(format_numeric(3, "0.##"), std::string("3"));
}

TEST(general_formats) {
    CHECK_EQ(eval("= 21 \\* roman"), std::string("xxi"));
    CHECK_EQ(eval("= 21 \\* ROMAN"), std::string("XXI"));
    CHECK_EQ(eval("= 3 \\* alphabetic"), std::string("c"));
    CHECK_EQ(eval("= 2 \\* ordinal"), std::string("2nd"));
    CHECK_EQ(eval("= 42 \\* cardtext"), std::string("forty-two"));
    CHECK_EQ(eval("= 1234 \\* CardText \\* FirstCap"), std::string("One thousand two hundred thirty-four"));
    CHECK_EQ(eval("= 3 \\* ordtext"), std::string("third"));
    CHECK_EQ(eval("= 255 \\* hex"), std::string("FF"));
    CHECK_EQ(eval("= 12.5 \\* dollartext"), std::string("twelve and 50/100"));
    CHECK_EQ(eval("QUOTE \"hello world\" \\* Upper"), std::string("HELLO WORLD"));
    CHECK_EQ(eval("QUOTE \"hello world\" \\* Caps"), std::string("Hello World"));
    CHECK_EQ(eval("QUOTE \"hello world\" \\* FirstCap"), std::string("Hello world"));
}

TEST(formulas) {
    CHECK_EQ(eval("= 2 + 3 * 4"), std::string("14"));
    CHECK_EQ(eval("= (2 + 3) * 4"), std::string("20"));
    CHECK_EQ(eval("= 2 ^ 10"), std::string("1024"));
    CHECK_EQ(eval("= 10 / 4"), std::string("2.5"));
    CHECK_EQ(eval("= SUM(1, 2, 3) \\# \"0.00\""), std::string("6.00"));
    CHECK_EQ(eval("= MAX(4, 9, 2)"), std::string("9"));
    CHECK_EQ(eval("= ROUND(2.345, 2)"), std::string("2.35"));
    CHECK_EQ(eval("= IF(3 > 2, 10, 20)"), std::string("10"));
    CHECK_EQ(eval("= 1 / 0"), std::string("!Syntax Error"));
    CHECK_EQ(eval("= 50%"), std::string("0.5"));
    CHECK_EQ(eval("=5*2"), std::string("10"));
}

TEST(if_and_compare) {
    CHECK_EQ(eval("IF 5 > 3 \"yes\" \"no\""), std::string("yes"));
    CHECK_EQ(eval("IF \"abc\" = \"a*\" \"match\" \"no\""), std::string("match"));
    CHECK_EQ(eval("IF \"abc\" <> \"abc\" \"diff\" \"same\""), std::string("same"));
    CHECK_EQ(eval("IF 10 <= 9 \"a\" \"b\""), std::string("b"));
    CHECK_EQ(eval("COMPARE 4 >= 4"), std::string("1"));
    CHECK_EQ(eval("COMPARE \"x\" = \"y\""), std::string("0"));
}

TEST(seq_and_nested_fields) {
    auto d = Document::create();
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"Figure ", nullptr, {});
    insert_field(d, p, 7, "SEQ Figure \\* ARABIC", "9", nullptr, {});
    auto q = insert_paragraph(d, p, true, U"Figure ", "", nullptr, {});
    insert_field(d, q, 7, "SEQ Figure \\* roman", "9", nullptr, {});
    auto r = insert_paragraph(d, q, true, U"Again ", "", nullptr, {});
    insert_field(d, r, 6, "SEQ Figure \\c", "9", nullptr, {});
    auto s = insert_paragraph(d, r, true, U"Reset ", "", nullptr, {});
    insert_field(d, s, 6, "SEQ Figure \\r 10", "9", nullptr, {});
    evaluate_fields(d, {}, nullptr);
    CHECK_EQ(analyze(p).utf8(), std::string("Figure 1"));
    CHECK_EQ(analyze(q).utf8(), std::string("Figure ii"));
    CHECK_EQ(analyze(r).utf8(), std::string("Again 2"));
    CHECK_EQ(analyze(s).utf8(), std::string("Reset 10"));
    CHECK_EQ(schema_issues(d), 0);
    save(d, "fields_seq");
}

TEST(properties_and_variables) {
    auto d = Document::create();
    set_core_properties(d, {{"title", "Annual Report"}, {"creator", "Ada Lovelace"}});
    set_custom_property(d, "Client", "Acme Corp");
    set_document_variable(d, "Version", "v2.1");
    CHECK_EQ(eval("TITLE", &d), std::string("Annual Report"));
    CHECK_EQ(eval("AUTHOR \\* Upper", &d), std::string("ADA LOVELACE"));
    CHECK_EQ(eval("DOCPROPERTY Client", &d), std::string("Acme Corp"));
    CHECK_EQ(eval("DOCPROPERTY Title", &d), std::string("Annual Report"));
    CHECK_EQ(eval("DOCVARIABLE Version", &d), std::string("v2.1"));
    CHECK_EQ(document_variables(d)["Version"].get<std::string>(), std::string("v2.1"));
    set_document_variable(d, "Version", nullptr);
    CHECK(document_variables(d).empty());
    CHECK_EQ(schema_issues(d), 0);
}

TEST(ref_and_set) {
    auto d = Document::create();
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"Total price", nullptr, {});
    add_bookmark(d, p, 6, p, 11, "price");
    auto q = insert_paragraph(d, p, true, U"See: ", "", nullptr, {});
    insert_field(d, q, 5, "REF price \\* Upper", "x", nullptr, {});
    auto r = insert_paragraph(d, q, true, U"", "", nullptr, {});
    insert_field(d, r, 0, "SET amount 250", "", nullptr, {});
    insert_field(d, r, 0, "= amount * 2 \\# \"$#,##0.00\"", "x", nullptr, {});
    evaluate_fields(d, {}, nullptr);
    CHECK_EQ(analyze(q).utf8(), std::string("See: PRICE"));
    CHECK(analyze(r).utf8().find("$500.00") != std::string::npos);
    CHECK_EQ(eval("REF nothing_here"), std::string("Error! Reference source not found."));
}

TEST(table_formula) {
    auto d = Document::create();
    auto tbl = insert_table(d, paragraphs(d.body())[0], true, {{"Item", "Cost"}, {"A", "1,200"}, {"B", "300.50"}, {"Total", ""}}, Json::object(), {});
    auto tc = cell(tbl, 3, 1);
    auto p = tc.child("w:p");
    insert_field(d, p, 0, "= SUM(ABOVE) \\# \"#,##0.00\"", "0", nullptr, {});
    auto tc2 = cell(tbl, 3, 0);
    insert_field(d, tc2.child("w:p"), (int)analyze(tc2.child("w:p")).vis.size(), "= B2 - B3", "0", nullptr, {});
    evaluate_fields(d, {}, nullptr);
    CHECK_EQ(analyze(p).utf8(), std::string("1,500.50"));
    CHECK_EQ(analyze(tc2.child("w:p")).utf8(), std::string("Total899.5"));
    CHECK_EQ(schema_issues(d), 0);
}

TEST(mail_merge_records) {
    auto d = Document::create();
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"Dear ", nullptr, {});
    insert_field(d, p, 5, "MERGEFIELD First", "\u00abFirst\u00bb", nullptr, {});
    insert_text(d, p, (int)analyze(p).vis.size(), U",", nullptr, {});
    auto q = insert_paragraph(d, p, true, U"", "", nullptr, {});
    insert_field(d, q, 0, "IF { MERGEFIELD Balance } > 100 \"You owe money.\" \"All good.\"", "x", nullptr, {});
    auto r = insert_paragraph(d, q, true, U"Due ", "", nullptr, {});
    insert_field(d, r, 4, "MERGEFIELD Due \\@ \"MMMM d, yyyy\"", "x", nullptr, {});
    auto s = insert_paragraph(d, r, true, U"", "", nullptr, {});
    insert_field(d, s, 0, "SKIPIF { MERGEFIELD First } = \"Skip\"", "", nullptr, {});
    Json rows = Json::array({{{"First", "Ann"}, {"Balance", 250}, {"Due", "2024-07-01"}},
                             {{"First", "Bob"}, {"Balance", "20"}, {"Due", "2024-08-15"}},
                             {{"First", "Skip"}, {"Balance", 0}, {"Due", ""}}});
    std::vector<std::string> notes;
    auto docs = mail_merge(d, rows, false, "", &notes);
    CHECK_EQ(docs.size(), std::size_t(2));
    if (docs.size() == 2) {
        auto ps = paragraphs(docs[0].body());
        CHECK_EQ(analyze(ps[0]).utf8(), std::string("Dear Ann,"));
        CHECK_EQ(analyze(ps[1]).utf8(), std::string("You owe money."));
        CHECK_EQ(analyze(ps[2]).utf8(), std::string("Due July 1, 2024"));
        auto ps2 = paragraphs(docs[1].body());
        CHECK_EQ(analyze(ps2[0]).utf8(), std::string("Dear Bob,"));
        CHECK_EQ(analyze(ps2[1]).utf8(), std::string("All good."));
        CHECK(list_fields(docs[0]).empty());
        CHECK_EQ(schema_issues(docs[0]), 0);
        save(docs[1], "merge_one");
    }
    CHECK_EQ(merge_field_names(d).size(), std::size_t(3));
    auto all = mail_merge(d, rows, true, "", nullptr);
    CHECK_EQ(all.size(), std::size_t(1));
    if (!all.empty()) {
        auto text = to_text(all[0], "current", false);
        CHECK(text.find("Dear Ann,") != std::string::npos);
        CHECK(text.find("Dear Bob,") != std::string::npos);
        CHECK(text.find("Skip") == std::string::npos);
        CHECK_EQ(sections(all[0]).size(), std::size_t(2));
        std::string why;
        CHECK_EQ(schema_issues(all[0], &why), 0);
        save(all[0], "merge_all");
    }
}

TEST(unlink_fields_to_text) {
    auto d = Document::create();
    auto p = paragraphs(d.body())[0];
    insert_text(d, p, 0, U"Page ", nullptr, {});
    insert_field(d, p, 5, "PAGE", "1", nullptr, {});
    insert_text(d, p, 6, U" of ", nullptr, {});
    insert_field(d, p, 10, "NUMPAGES", "3", nullptr, {});
    CHECK_EQ(unlink_fields(d, {"page"}), 1);
    CHECK_EQ(list_fields(d).size(), std::size_t(1));
    CHECK_EQ(analyze(p).utf8(), std::string("Page 1 of 3"));
    CHECK_EQ(unlink_fields(d, {}), 1);
    CHECK(list_fields(d).empty());
    CHECK_EQ(analyze(p).utf8(), std::string("Page 1 of 3"));
    CHECK_EQ(schema_issues(d), 0);
}

TEST(csv_loading) {
    std::filesystem::create_directories("out");
    std::FILE* f = std::fopen("out/data.csv", "wb");
    std::fputs("\xEF\xBB\xBFName,City,Note\r\nAnn,\"Paris, FR\",\"said \"\"hi\"\"\"\r\nBob,Rome,\r\n", f);
    std::fclose(f);
    auto rows = load_merge_data("out/data.csv");
    CHECK_EQ(rows.size(), std::size_t(2));
    CHECK_EQ(rows[0]["City"].get<std::string>(), std::string("Paris, FR"));
    CHECK_EQ(rows[0]["Note"].get<std::string>(), std::string("said \"hi\""));
    CHECK_EQ(rows[1]["Note"].get<std::string>(), std::string(""));
}

// Real page numbers need LibreOffice; skipped without it.
TEST(page_numbers_from_layout) {
    if (renderer().empty()) { std::printf("  (no LibreOffice: skipped)\n"); return; }
    auto d = Document::create();
    auto last = paragraphs(d.body())[0];
    insert_text(d, last, 0, U"Start", nullptr, {});
    for (int i = 0; i < 3; i++) {
        last = insert_paragraph(d, last, true, U"Chapter " + util::to_u32(std::to_string(i + 1)), "Heading1", nullptr, {});
        insert_break(d, last, 0, "page", {});
        for (int k = 0; k < 3; k++) last = insert_paragraph(d, last, true, U"Body text.", "", nullptr, {});
    }
    auto h3 = paragraphs(d.body())[9];
    add_bookmark(d, h3, 0, h3, 3, "third");
    auto q = insert_paragraph(d, last, true, U"See page ", "", nullptr, {});
    insert_field(d, q, 9, "PAGEREF third", "?", nullptr, {});
    insert_text(d, q, (int)analyze(q).vis.size(), U" of ", nullptr, {});
    insert_field(d, q, (int)analyze(q).vis.size(), "NUMPAGES", "?", nullptr, {});
    insert_text(d, q, (int)analyze(q).vis.size(), U", this is page ", nullptr, {});
    insert_field(d, q, (int)analyze(q).vis.size(), "PAGE \\* roman", "?", nullptr, {});
    FieldUpdate u;
    u.layout = true;
    auto r = update_fields(d, u);
    CHECK(r.pages);
    CHECK_EQ(analyze(q).utf8(), std::string("See page 4 of 4, this is page iv"));
    auto pm = page_map(d);
    CHECK_EQ(pm.total, 4);
    CHECK_EQ(pm.paras.front(), 1);
    CHECK_EQ(schema_issues(d), 0);
    save(d, "fields_pages");
}

int main(int argc, char** argv) { return t::run(argc, argv); }

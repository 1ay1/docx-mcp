// References > Citations & Bibliography: the source store Word keeps in a
// custom XML part (b:Sources), CITATION fields in the text, and a
// BIBLIOGRAPHY field listing every cited source. Results are written in APA,
// MLA, Chicago or IEEE style so the file reads right before Word refreshes.
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "util/util.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/fields_internal.hpp"
#include "wml/render.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

constexpr const char* kBibNs = "http://schemas.openxmlformats.org/officeDocument/2006/bibliography";

// The custom XML part holding b:Sources, created on demand.
std::string sources_part(Document& d, bool create) {
    if (const auto* rels = d.pkg().rels_if(d.main())) {
        for (auto& r : rels->items) {
            if (r.type != opc::rt::customXml) continue;
            std::string part = opc::Package::resolve(d.main(), r.target);
            if (!d.pkg().has(part)) continue;
            auto& x = d.pkg().xml(part);
            auto root = x.document_element();
            std::string n = root.name();
            if (n == "b:Sources" || (n.size() >= 8 && n.substr(n.find(':') + 1) == "Sources")) return part;
        }
    }
    if (!create) return "";
    std::string item = d.pkg().unique_name("/customXml/item", ".xml");
    std::string props = item;
    props.replace(props.rfind("item"), 4, "itemProps");
    std::string xmltext = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<b:Sources SelectedStyle=\"\\APASixthEditionOfficeOnline.xsl\" "
                          "StyleName=\"APA\" Version=\"6\" xmlns:b=\"" + std::string(kBibNs) +
                          "\" xmlns=\"" + kBibNs + "\"></b:Sources>";
    d.pkg().add_part(item, "application/xml", util::Bytes(xmltext.begin(), xmltext.end()));
    std::string pxml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\r\n<ds:datastoreItem ds:itemID=\"" + util::new_guid() +
                       "\" xmlns:ds=\"http://schemas.openxmlformats.org/officeDocument/2006/customXml\"><ds:schemaRefs><ds:schemaRef ds:uri=\"" +
                       kBibNs + "\"/></ds:schemaRefs></ds:datastoreItem>";
    d.pkg().add_xml_part(props, opc::ct::customXmlProps, pxml);
    d.pkg().rels(item).add(opc::rt::customXmlProps, opc::Package::relative(item, props));
    d.pkg().rels(d.main()).add(opc::rt::customXml, opc::Package::relative(d.main(), item));
    return item;
}

// Child of a b:Source by local name (prefix may be b: or none).
pugi::xml_node kid(pugi::xml_node n, const char* local) {
    for (auto c : n.children()) {
        std::string q = c.name();
        auto colon = q.find(':');
        if ((colon == std::string::npos ? q : q.substr(colon + 1)) == local) return c;
    }
    return {};
}

std::string kid_text(pugi::xml_node n, const char* local) { return kid(n, local).text().get(); }

std::string prefix_of(pugi::xml_node root) {
    std::string q = root.name();
    auto c = q.find(':');
    return c == std::string::npos ? "" : q.substr(0, c + 1);
}

struct Person { std::string last, first, middle; };

std::vector<Person> people(pugi::xml_node src, const char* role = "Author") {
    std::vector<Person> v;
    auto auth = kid(kid(src, "Author"), role);
    for (auto list : auth.children()) {
        std::string q = list.name();
        if (q.find("NameList") == std::string::npos) continue;
        for (auto p : list.children()) {
            Person x{kid_text(p, "Last"), kid_text(p, "First"), kid_text(p, "Middle")};
            if (!x.last.empty() || !x.first.empty()) v.push_back(x);
        }
    }
    if (v.empty()) {
        // corporate author
        std::string corp = kid_text(auth, "Corporate");
        if (!corp.empty()) v.push_back({corp, "", ""});
    }
    return v;
}

std::string initials(const Person& p) {
    std::string o;
    for (auto& part : {p.first, p.middle}) {
        bool start = true;
        for (char c : part) {
            if (c == ' ' || c == '-') { start = true; continue; }
            if (start && std::isalpha((unsigned char)c)) { o += (char)std::toupper((unsigned char)c); o += ". "; }
            start = false;
        }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::string join_names(const std::vector<Person>& ps, const std::string& style) {
    std::vector<std::string> n;
    for (std::size_t i = 0; i < ps.size(); i++) {
        auto& p = ps[i];
        if (p.first.empty()) { n.push_back(p.last); continue; }
        if (style == "APA") n.push_back(p.last + ", " + initials(p));
        else if (style == "IEEE") n.push_back(initials(p) + " " + p.last);
        else if (i == 0) n.push_back(p.last + ", " + p.first + (p.middle.empty() ? "" : " " + p.middle));
        else n.push_back(p.first + (p.middle.empty() ? "" : " " + p.middle) + " " + p.last);
    }
    if (n.empty()) return "";
    if (n.size() == 1) return n[0];
    std::string amp = style == "APA" ? ", & " : (style == "IEEE" ? ", and " : ", and ");
    if (n.size() == 2) return n[0] + (style == "APA" ? ", & " : " and ") + n[1];
    std::string o;
    for (std::size_t i = 0; i + 1 < n.size(); i++) o += (i ? ", " : "") + n[i];
    return o + amp + n.back();
}

// In-text citation: (Smith, 2020) / (Smith 12) / [1].
std::string cite_text(pugi::xml_node src, const std::string& style, int number, const std::string& pages) {
    auto ps = people(src);
    std::string who;
    if (ps.empty()) who = kid_text(src, "Title");
    else if (ps.size() == 1) who = ps[0].last;
    else if (ps.size() == 2) who = ps[0].last + (style == "APA" ? " & " : " and ") + ps[1].last;
    else who = ps[0].last + " et al.";
    std::string year = kid_text(src, "Year");
    if (style == "IEEE") return "[" + std::to_string(number) + "]";
    if (style == "MLA") return "(" + who + (pages.empty() ? "" : " " + pages) + ")";
    if (style == "Chicago") return "(" + who + (year.empty() ? "" : " " + year) + (pages.empty() ? "" : ", " + pages) + ")";
    return "(" + who + ", " + (year.empty() ? "n.d." : year) + (pages.empty() ? "" : ", p. " + pages) + ")";
}

// Bibliography entry; italics marked with \x01 ... \x02 for the title.
std::string entry_text(pugi::xml_node src, const std::string& style, int number) {
    std::string type = kid_text(src, "SourceType");
    auto ps = people(src);
    std::string names = join_names(ps, style);
    std::string year = kid_text(src, "Year"), title = kid_text(src, "Title"), city = kid_text(src, "City"),
                pub = kid_text(src, "Publisher"), journal = kid_text(src, "JournalName"), vol = kid_text(src, "Volume"),
                issue = kid_text(src, "Issue"), pages = kid_text(src, "Pages"), url = kid_text(src, "URL"),
                site = kid_text(src, "InternetSiteTitle"), book = kid_text(src, "BookTitle");
    bool container = type == "JournalArticle" || type == "ArticleInAPeriodical" || type == "BookSection" || type == "ConferenceProceedings";
    std::string ital_title = container ? title : "\x01" + title + "\x02";
    std::string where = container ? (journal.empty() ? book : journal) : "";
    std::string o;
    if (style == "APA") {
        o = (names.empty() ? "" : names + " ") + "(" + (year.empty() ? "n.d." : year) + "). " + ital_title + ". ";
        if (!where.empty()) {
            o += "\x01" + where + (vol.empty() ? "" : ", " + vol) + "\x02" + (issue.empty() ? "" : "(" + issue + ")") + (pages.empty() ? "" : ", " + pages) + ". ";
        } else if (!pub.empty()) o += pub + ". ";
        if (!url.empty()) o += url;
    } else if (style == "MLA") {
        o = (names.empty() ? "" : names + ". ") + (container ? "\"" + title + ".\" " : ital_title + ". ");
        if (!where.empty()) o += "\x01" + where + "\x02" + (vol.empty() ? "" : ", vol. " + vol) + (issue.empty() ? "" : ", no. " + issue) + ", ";
        if (!pub.empty()) o += pub + ", ";
        o += year.empty() ? "" : year;
        if (!pages.empty()) o += ", pp. " + pages;
        o += ".";
        if (!url.empty()) o += " " + url + ".";
    } else if (style == "Chicago") {
        o = (names.empty() ? "" : names + ". ") + (year.empty() ? "" : year + ". ") + (container ? "\"" + title + ".\" " : ital_title + ". ");
        if (!where.empty()) o += "\x01" + where + "\x02" + (vol.empty() ? "" : " " + vol) + (issue.empty() ? "" : " (" + issue + ")") + (pages.empty() ? "" : ": " + pages) + ". ";
        if (!city.empty() || !pub.empty()) o += city + (city.empty() || pub.empty() ? "" : ": ") + pub + ". ";
        if (!url.empty()) o += url + ".";
    } else {  // IEEE
        o = "[" + std::to_string(number) + "] " + (names.empty() ? "" : names + ", ") + (container ? "\"" + title + ",\" " : ital_title + ", ");
        if (!where.empty()) o += "\x01" + where + "\x02" + (vol.empty() ? "" : ", vol. " + vol) + (issue.empty() ? "" : ", no. " + issue) + (pages.empty() ? "" : ", pp. " + pages) + ", ";
        if (!city.empty()) o += city + ": ";
        if (!pub.empty()) o += pub + ", ";
        o += year.empty() ? "" : year;
        o += ".";
        if (!url.empty()) o += " [Online]. Available: " + url;
    }
    return util::trim(o);
}

std::string style_name(Document& d, const std::string& want) {
    if (!want.empty()) {
        std::string s = util::upper(want);
        if (s == "APA") return "APA";
        if (s == "MLA") return "MLA";
        if (s == "CHICAGO") return "Chicago";
        if (s == "IEEE") return "IEEE";
        throw std::runtime_error("citation style is APA, MLA, Chicago or IEEE");
    }
    auto part = sources_part(d, false);
    if (part.empty()) return "APA";
    std::string s = xml::attr(d.pkg().xml(part).document_element(), "StyleName");
    std::string u = util::upper(s);
    if (u.find("MLA") != std::string::npos) return "MLA";
    if (u.find("CHICAGO") != std::string::npos) return "Chicago";
    if (u.find("IEEE") != std::string::npos) return "IEEE";
    return "APA";
}

pugi::xml_node find_source(Document& d, const std::string& tag) {
    auto part = sources_part(d, false);
    if (part.empty()) return {};
    for (auto s : d.pkg().xml(part).document_element().children())
        if (util::iequals(kid_text(s, "Tag"), tag)) return s;
    return {};
}

// Order sources are cited in the body (for IEEE numbering).
std::vector<std::string> cited_tags(Document& d) {
    std::vector<std::string> v;
    for (auto a : field_anchors(d.body())) {
        std::string code = detail::field_code(a);
        auto t = util::split(util::trim(code), ' ');
        if (t.size() >= 2 && util::iequals(t[0], "CITATION")) {
            std::string tag = t[1];
            if (std::find_if(v.begin(), v.end(), [&](auto& x) { return util::iequals(x, tag); }) == v.end()) v.push_back(tag);
        }
    }
    return v;
}

void write_runs(Document& d, pugi::xml_node p, const std::string& text) {
    // \x01 .. \x02 is italic
    std::string cur;
    bool ital = false;
    auto flush = [&]() {
        if (cur.empty()) return;
        auto r = p.append_child("w:r");
        if (ital) apply_rpr_json(d, r.append_child("w:rPr"), Json{{"italic", true}});
        append_text(r, util::to_u32(cur));
        cur.clear();
    };
    for (char c : text) {
        if (c == '\x01') { flush(); ital = true; continue; }
        if (c == '\x02') { flush(); ital = false; continue; }
        cur += c;
    }
    flush();
}

}  // namespace

Json list_sources(Document& d) {
    Json out = Json::array();
    auto part = sources_part(d, false);
    if (part.empty()) return out;
    auto cited = cited_tags(d);
    for (auto s : d.pkg().xml(part).document_element().children()) {
        if (s.type() != pugi::node_element) continue;
        Json j = {{"tag", kid_text(s, "Tag")}, {"type", kid_text(s, "SourceType")}, {"title", kid_text(s, "Title")}, {"year", kid_text(s, "Year")}};
        Json authors = Json::array();
        for (auto& p : people(s)) authors.push_back(p.first.empty() ? p.last : p.first + (p.middle.empty() ? "" : " " + p.middle) + " " + p.last);
        j["authors"] = authors;
        for (auto [k, local] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"publisher", "Publisher"}, {"city", "City"}, {"journal", "JournalName"}, {"volume", "Volume"}, {"issue", "Issue"},
                 {"pages", "Pages"}, {"url", "URL"}, {"book_title", "BookTitle"}, {"site", "InternetSiteTitle"}})
            if (auto t = kid_text(s, local); !t.empty()) j[k] = t;
        bool c = std::find_if(cited.begin(), cited.end(), [&](auto& x) { return util::iequals(x, kid_text(s, "Tag")); }) != cited.end();
        j["cited"] = c;
        out.push_back(j);
    }
    return out;
}

std::string add_source(Document& d, const Json& src) {
    static const std::map<std::string, std::string> types = {
        {"book", "Book"}, {"article", "JournalArticle"}, {"journal", "JournalArticle"}, {"journal_article", "JournalArticle"},
        {"website", "InternetSite"}, {"web", "InternetSite"}, {"webpage", "DocumentFromInternetSite"}, {"report", "Report"},
        {"conference", "ConferenceProceedings"}, {"chapter", "BookSection"}, {"book_section", "BookSection"},
        {"magazine", "ArticleInAPeriodical"}, {"newspaper", "ArticleInAPeriodical"}, {"thesis", "Report"}, {"film", "Film"},
        {"patent", "Patent"}, {"case", "Case"}, {"misc", "Misc"}, {"interview", "Interview"}, {"sound", "SoundRecording"}};
    std::string t = util::lower(src.value("type", std::string("book")));
    auto it = types.find(t);
    if (it == types.end()) {
        std::vector<std::string_view> valid;
        for (auto& [k, v] : types) valid.push_back(k);
        throw std::runtime_error(util::unknown_key("source type", t, valid));
    }
    std::string title = src.value("title", std::string());
    if (util::trim(title).empty()) throw std::runtime_error("a source needs a title");
    // tag: given, else LastYear like Word (Smi20 style is older; Word 365 uses Smith20)
    std::string tag = src.value("tag", std::string());
    std::vector<Person> authors;
    if (src.contains("authors")) {
        for (auto& a : src["authors"]) {
            if (a.is_object()) { authors.push_back({a.value("last", std::string()), a.value("first", std::string()), a.value("middle", std::string())}); continue; }
            std::string s = util::trim(a.get<std::string>());
            if (s.empty()) continue;
            // "Last, First Middle" or "First Middle Last"
            Person p;
            if (auto c = s.find(','); c != std::string::npos) {
                p.last = util::trim(s.substr(0, c));
                auto rest = util::split(util::trim(s.substr(c + 1)), ' ');
                if (!rest.empty()) p.first = rest[0];
                for (std::size_t k = 1; k < rest.size(); k++) p.middle += (p.middle.empty() ? "" : " ") + rest[k];
            } else {
                auto w = util::split(s, ' ');
                std::vector<std::string> ws;
                for (auto& x : w) if (!x.empty()) ws.push_back(x);
                p.last = ws.back();
                if (ws.size() > 1) p.first = ws[0];
                for (std::size_t k = 1; k + 1 < ws.size(); k++) p.middle += (p.middle.empty() ? "" : " ") + ws[k];
            }
            authors.push_back(p);
        }
    }
    std::string year = src.contains("year") ? (src["year"].is_string() ? src["year"].get<std::string>() : src["year"].dump()) : "";
    if (tag.empty()) {
        std::string base = authors.empty() ? title.substr(0, std::min<std::size_t>(title.size(), 8)) : authors[0].last;
        for (auto& c : base) if (!std::isalnum((unsigned char)c)) c = '_';
        tag = base + (year.size() >= 2 ? year.substr(year.size() - 2) : "");
        std::string t0 = tag;
        for (int k = 2; find_source(d, tag); k++) tag = t0 + std::to_string(k);
    } else if (find_source(d, tag)) {
        throw std::runtime_error("a source with tag '" + tag + "' exists (remove_source first, or pick another tag)");
    }
    for (char c : tag) if (std::isspace((unsigned char)c)) throw std::runtime_error("a source tag has no spaces");
    auto part = sources_part(d, true);
    auto root = d.pkg().xml(part).document_element();
    std::string px = prefix_of(root);
    auto el = [&](pugi::xml_node parent, const char* local) { return parent.append_child((px + local).c_str()); };
    auto s = el(root, "Source");
    el(s, "Tag").text().set(tag.c_str());
    el(s, "SourceType").text().set(it->second.c_str());
    el(s, "Guid").text().set(util::new_guid().c_str());
    if (!authors.empty()) {
        auto nl = el(el(el(s, "Author"), "Author"), "NameList");
        for (auto& a : authors) {
            auto pn = el(nl, "Person");
            if (!a.last.empty()) el(pn, "Last").text().set(a.last.c_str());
            if (!a.first.empty()) el(pn, "First").text().set(a.first.c_str());
            if (!a.middle.empty()) el(pn, "Middle").text().set(a.middle.c_str());
        }
    } else if (src.contains("corporate_author")) {
        el(el(el(s, "Author"), "Author"), "Corporate").text().set(src["corporate_author"].get<std::string>().c_str());
    }
    el(s, "Title").text().set(title.c_str());
    if (!year.empty()) el(s, "Year").text().set(year.c_str());
    for (auto [k, local] : std::initializer_list<std::pair<const char*, const char*>>{
             {"publisher", "Publisher"}, {"city", "City"}, {"journal", "JournalName"}, {"volume", "Volume"}, {"issue", "Issue"},
             {"pages", "Pages"}, {"url", "URL"}, {"book_title", "BookTitle"}, {"site", "InternetSiteTitle"}, {"month", "Month"},
             {"day", "Day"}, {"edition", "Edition"}, {"doi", "DOI"}, {"institution", "Institution"}})
        if (src.contains(k)) {
            std::string v = src[k].is_string() ? src[k].get<std::string>() : src[k].dump();
            el(s, local).text().set(v.c_str());
        }
    return tag;
}

bool remove_source(Document& d, const std::string& tag) {
    auto s = find_source(d, tag);
    if (!s) return false;
    s.parent().remove_child(s);
    return true;
}

void insert_citation(Document& d, pugi::xml_node p, int pos, const std::string& tag, const std::string& pages, const EditOpts& o) {
    auto s = find_source(d, tag);
    if (!s) throw std::runtime_error("no source with tag '" + tag + "' (add_source first; list_sources shows them)");
    std::string real = kid_text(s, "Tag");
    std::string code = "CITATION " + real + " \\l 1033";
    if (!pages.empty()) code += " \\p " + pages;
    std::string style = style_name(d, "");
    auto tags = cited_tags(d);
    int num = (int)tags.size() + 1;
    for (std::size_t i = 0; i < tags.size(); i++) if (util::iequals(tags[i], real)) num = (int)i + 1;
    insert_field(d, p, pos, code, cite_text(s, style, num, pages), nullptr, o);
    // Word wraps citations in a content control; the field alone is valid and Word accepts it
}

int insert_bibliography(Document& d, pugi::xml_node after, const std::string& title, const EditOpts& o) {
    // a heading, then one paragraph per source with the field around them all
    auto part = sources_part(d, false);
    if (part.empty()) throw std::runtime_error("no sources yet (add_source first)");
    std::string style = style_name(d, "");
    pugi::xml_node last = after;
    if (!title.empty()) last = insert_paragraph(d, last, true, util::to_u32(title), ensure_style(d, "Heading1"), nullptr, o);
    auto bp = insert_paragraph(d, last, true, U"", ensure_style(d, "Bibliography"), nullptr, o);
    insert_field(d, bp, 0, "BIBLIOGRAPHY", "", nullptr, o);
    rebuild_bibliographies(d, style);
    int n = 0;
    for (auto s : d.pkg().xml(part).document_element().children()) if (s.type() == pugi::node_element) n++;
    return n;
}

int rebuild_bibliographies(Document& d, const std::string& want_style) {
    auto part = sources_part(d, false);
    if (part.empty()) return 0;
    std::string style = style_name(d, want_style);
    auto root = d.pkg().xml(part).document_element();
    // cited first in citation order (IEEE), the rest after; APA/MLA/Chicago sort by author
    auto cited = cited_tags(d);
    std::vector<pugi::xml_node> srcs;
    for (auto s : root.children()) if (s.type() == pugi::node_element) srcs.push_back(s);
    std::vector<std::pair<std::string, pugi::xml_node>> order;
    for (auto s : srcs) {
        std::string key;
        if (style == "IEEE") {
            auto it = std::find_if(cited.begin(), cited.end(), [&](auto& x) { return util::iequals(x, kid_text(s, "Tag")); });
            char b[16];
            std::snprintf(b, sizeof b, "%06d", it == cited.end() ? 999999 : (int)(it - cited.begin()));
            key = b;
        } else {
            auto ps = people(s);
            key = util::lower((ps.empty() ? kid_text(s, "Title") : ps[0].last) + " " + kid_text(s, "Year") + " " + kid_text(s, "Title"));
        }
        order.push_back({key, s});
    }
    std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first < b.first; });
    int rebuilt = 0;
    for (auto a : field_anchors(d.body())) {
        if (!util::starts_with(util::upper(detail::field_code(a)), "BIBLIOGRAPHY")) continue;
        if (xml::name_is(a, "w:fldSimple")) continue;
        auto end = detail::field_end(a);
        if (!end) continue;
        auto p1 = xml::ancestor(a, "w:p");
        auto p2 = xml::ancestor(end, "w:p");
        // separate mark in the first paragraph
        pugi::xml_node sep;
        for (auto r : xml::children(p1, "w:r"))
            if (auto f = r.child("w:fldChar"); f && xml::attr(f, "w:fldCharType") == "separate") sep = r;
        if (!sep) continue;
        // drop the old result: paragraphs between p1 and p2 (only when p2 really follows p1 as a sibling)
        std::vector<pugi::xml_node> kill;
        if (p2 != p1 && p2.parent() == p1.parent()) {
            bool reaches = false;
            for (auto x = p1.next_sibling(); x; x = x.next_sibling())
                if (x == p2) { reaches = true; break; }
            if (reaches)
                for (auto x = p1.next_sibling(); x && x != p2; x = x.next_sibling())
                    if (!xml::name_is(x, "w:sectPr")) kill.push_back(x);
        }
        for (auto x : kill) x.parent().remove_child(x);
        std::vector<pugi::xml_node> drop;
        for (auto x = sep.next_sibling(); x; x = x.next_sibling()) {
            if (x == end.parent()) break;
            drop.push_back(x);
        }
        for (auto x : drop) x.parent().remove_child(x);
        auto end_run = end.parent();
        if (p2 != p1) {
            // runs before the end mark in its paragraph are old result too
            std::vector<pugi::xml_node> old;
            for (auto x = p2.first_child(); x && x != end_run; x = x.next_sibling())
                if (!xml::name_is(x, "w:pPr")) old.push_back(x);
            for (auto x : old) p2.remove_child(x);
            // move the end mark back into the first paragraph and drop p2 if it's now empty
            p1.append_move(end_run);
            bool empty = true;
            for (auto c : p2.children()) if (!xml::name_is(c, "w:pPr")) empty = false;
            if (empty) p2.parent().remove_child(p2);
        }
        // entries: the first goes in p1 (before the end mark), the rest as new paragraphs
        int num = 1;
        pugi::xml_node cur = p1;
        for (std::size_t i = 0; i < order.size(); i++) {
            std::string text = entry_text(order[i].second, style, num++);
            if (i == 0) {
                pugi::xml_document tmp;
                auto holder = tmp.append_child("w:p");
                write_runs(d, holder, text);
                for (auto r : holder.children()) p1.insert_copy_before(r, end_run);
            } else {
                auto np = p1.parent().insert_child_after("w:p", cur);
                if (auto ppr = p1.child("w:pPr")) np.append_copy(ppr);
                write_runs(d, np, text);
                cur = np;
            }
        }
        if (order.empty()) {
            auto r = p1.insert_child_before("w:r", end_run);
            append_text(r, U"There are no sources in the current document.");
        }
        // the end mark belongs after the last entry
        if (cur != p1) cur.append_move(end_run);
        d.ensure_para_ids(p1.parent());
        rebuilt++;
    }
    // refresh citation results too
    for (auto a : field_anchors(d.body())) {
        std::string code = detail::field_code(a);
        auto t = util::split(util::trim(code), ' ');
        if (t.size() < 2 || !util::iequals(t[0], "CITATION")) continue;
        auto s = find_source(d, t[1]);
        std::string pages;
        for (std::size_t k = 2; k + 1 < t.size(); k++) if (t[k] == "\\p") pages = t[k + 1];
        int num = 1;
        for (std::size_t i = 0; i < cited.size(); i++) if (util::iequals(cited[i], t[1])) num = (int)i + 1;
        std::string res = s ? cite_text(s, style, num, pages) : "(Source not found)";
        if (xml::name_is(a, "w:fldSimple")) continue;
        auto end = detail::field_end(a);
        auto p = xml::ancestor(a, "w:p");
        if (!end || xml::ancestor(end, "w:p") != p) continue;
        pugi::xml_node sep;
        for (auto x = a.parent(); x && x != end.parent(); x = x.next_sibling())
            if (auto f = x.child("w:fldChar"); f && xml::attr(f, "w:fldCharType") == "separate") sep = x;
        if (!sep) continue;
        pugi::xml_node first;
        std::vector<pugi::xml_node> drop;
        for (auto x = sep.next_sibling(); x && x != end.parent(); x = x.next_sibling()) {
            if (!first && x.child("w:t")) first = x;
            else drop.push_back(x);
        }
        for (auto x : drop) x.parent().remove_child(x);
        if (!first) first = p.insert_child_before("w:r", end.parent());
        for (auto t2 : xml::children(first, "w:t")) first.remove_child(t2);
        append_text(first, util::to_u32(res));
    }
    // the selected style
    xml::set_attr(root, "StyleName", style);
    static const std::map<std::string, std::string> xsl = {{"APA", "\\APASixthEditionOfficeOnline.xsl"}, {"MLA", "\\MLASeventhEditionOfficeOnline.xsl"},
                                                           {"Chicago", "\\CHICAGO.XSL"}, {"IEEE", "\\IEEE2006OfficeOnline.xsl"}};
    xml::set_attr(root, "SelectedStyle", xsl.at(style));
    return rebuilt;
}

}  // namespace docx::wml

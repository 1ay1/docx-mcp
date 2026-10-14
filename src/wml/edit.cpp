#include <map>
#include <algorithm>
#include <functional>
#include <regex>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/edit.hpp"
#include "wml/styles.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool name_is(pugi::xml_node n, const char* q) { return std::string_view(n.name()) == q; }

bool tracking(Document& d, const EditOpts& o) { return o.track ? *o.track : d.track_changes(); }

// Revision container directly wrapping `run` (or null).
pugi::xml_node container_of(pugi::xml_node run) {
    auto p = run.parent();
    return is_revision_container(p) ? p : pugi::xml_node{};
}

bool mine(Document& d, pugi::xml_node rev) { return rev && xml::attr(rev, "w:author") == d.author.name; }

// Wrap `node` in a new revision container `q` (w:ins/w:del) in place.
pugi::xml_node wrap(Document& d, pugi::xml_node node, const char* q) {
    auto parent = node.parent();
    auto c = parent.insert_child_before(q, node);
    stamp(d, c);
    c.append_move(node);
    return c;
}

// Convert a run's text to deleted form (w:t -> w:delText, instrText -> delInstrText).
void to_deleted(pugi::xml_node run) {
    for (auto c : run.children()) {
        if (name_is(c, "w:t")) c.set_name("w:delText");
        else if (name_is(c, "w:instrText")) c.set_name("w:delInstrText");
    }
}
// Visible length of paragraph.
int vlen(pugi::xml_node p) { return (int)analyze(p).vis.size(); }

// The insertion point for new content at visible position pos: the node to
// insert before (in its parent), plus the run whose formatting to inherit.
struct InsPoint {
    pugi::xml_node parent;
    pugi::xml_node before;  // null = append
    pugi::xml_node fmt_run; // run to inherit rPr from
};

InsPoint insertion_point(pugi::xml_node p, int pos) {
    split_at(p, pos);
    ParaText pt = analyze(p);
    InsPoint ip;
    int n = (int)pt.vis.size();
    if (pos < n) {
        const Atom& a = pt.atoms[pt.vis[pos]];
        pugi::xml_node node = a.run;
        // climb to a child of a "text container" (p, hyperlink, sdtContent, smartTag...)
        // stopping above revision containers so we insert beside them.
        while (is_revision_container(node.parent())) node = node.parent();
        ip.parent = node.parent();
        ip.before = node;
        if (pos > 0) ip.fmt_run = pt.atoms[pt.vis[pos - 1]].run;
        else ip.fmt_run = a.run;
    } else {
        // end of paragraph: after the last visible atom's run, else append to p
        if (n > 0) {
            const Atom& a = pt.atoms[pt.vis[n - 1]];
            pugi::xml_node node = a.run;
            while (is_revision_container(node.parent())) node = node.parent();
            ip.parent = node.parent();
            // the last visible text may be a complex field's result: new text
            // goes after the field's end mark, not inside the result
            ip.before = past_field_result(node);
            ip.fmt_run = a.run;
            // don't put text after the last paragraph-level bookmark/comment ends inside a hyperlink
        } else {
            ip.parent = p;
            ip.before = {};
        }
    }
    if (ip.fmt_run && !name_is(ip.fmt_run, "w:r")) ip.fmt_run = {};
    return ip;
}

}  // namespace

void stamp(Document& d, pugi::xml_node rev) {
    xml::set_attr(rev, "w:id", std::to_string(d.next_annotation_id()));
    xml::set_attr(rev, "w:author", d.author.name);
    xml::set_attr(rev, "w:date", d.date());
    // order: id, author, date (Word's order)
    std::vector<std::pair<std::string, std::string>> attrs;
    for (auto a : rev.attributes()) attrs.emplace_back(a.name(), a.value());
    while (rev.first_attribute()) rev.remove_attribute(rev.first_attribute());
    for (auto key : {"w:id", "w:author", "w:date"})
        for (auto& [k, v] : attrs) if (k == key) rev.append_attribute(k.c_str()) = v.c_str();
    for (auto& [k, v] : attrs)
        if (k != "w:id" && k != "w:author" && k != "w:date") rev.append_attribute(k.c_str()) = v.c_str();
    d.ensure_person(d.author.name);
}

// ── text ─────────────────────────────────────────────────────────────────

void mark_paragraph_inserted(Document& d, pugi::xml_node p, const char* q_mark) {
    // Copied content carries its old revisions: deleted text is gone in the
    // copy, inserted text is plain content of the new insertion.
    std::function<void(pugi::xml_node)> settle = [&](pugi::xml_node n) {
        for (auto c = n.first_child(); c;) {
            auto nx = c.next_sibling();
            std::string_view q = c.name();
            if (q == "w:del" || q == "w:moveFrom") {
                n.remove_child(c);
            } else if (q == "w:ins" || q == "w:moveTo") {
                // lift the content, then visit it (it may hold refs and ranges too)
                nx = c.first_child() ? c.first_child() : c.next_sibling();
                for (auto k = c.first_child(); k;) {
                    auto kn = k.next_sibling();
                    n.insert_move_before(k, c);
                    k = kn;
                }
                n.remove_child(c);
            } else if (q == "w:moveFromRangeStart" || q == "w:moveFromRangeEnd" || q == "w:moveToRangeStart" ||
                       q == "w:moveToRangeEnd" || q == "w:commentRangeStart" || q == "w:commentRangeEnd" ||
                       q == "w:bookmarkStart" || q == "w:bookmarkEnd" || q == "w:proofErr" ||
                       q == "w:permStart" || q == "w:permEnd") {
                // ranges belong to the original; the copy doesn't own them
                n.remove_child(c);
            } else if (q == "w:r") {
                // a comment reference is the original's too
                if (c.child("w:commentReference")) n.remove_child(c);
                else for (auto k : xml::descendants(c, "w:rPrChange")) k.parent().remove_child(k);
            } else if (q != "w:pPr") {
                settle(c);
            }
            c = nx;
        }
    };
    settle(p);
    auto rpr = schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:rPr");
    for (auto q : {"w:ins", "w:del", "w:moveFrom", "w:moveTo"}) rpr.remove_child(q);
    stamp(d, schema::ensure_child(rpr, q_mark));
    p.child("w:pPr").remove_child("w:pPrChange");
    // wrap runs (and math) where they sit: in p, hyperlinks, fldSimple, sdtContent...
    std::function<void(pugi::xml_node)> wrap_runs = [&](pugi::xml_node n) {
        std::vector<pugi::xml_node> run;
        auto flush = [&]() {
            if (run.empty()) return;
            auto ins = n.insert_child_before(q_mark, run.front());
            stamp(d, ins);
            for (auto r : run) ins.append_move(r);
            run.clear();
        };
        for (auto c = n.first_child(); c;) {
            auto nx = c.next_sibling();
            std::string_view q = c.name();
            if (q == "w:r" || q == "m:oMath" || q == "m:oMathPara") {
                if (q == "w:r") run.push_back(c);
                else { flush(); for (auto mr : xml::descendants(c, "m:r")) wrap_math_run(d, mr, q_mark); }
            } else {
                flush();
                if (q == "w:hyperlink" || q == "w:fldSimple" || q == "w:smartTag" || q == "w:customXml" ||
                    q == "w:sdtContent" || q == "w:sdt" || q == "w:dir" || q == "w:bdo")
                    wrap_runs(c);
            }
            c = nx;
        }
        flush();
    };
    wrap_runs(p);
}

void wrap_math_run(Document& d, pugi::xml_node mr, const char* q) {
    // ECMA CT_RunTrackChange allows math content (EG_OMathMathElements), so the
    // valid form is <w:ins><m:r>..</m:r></w:ins>. Word itself writes the
    // marker inside m:r, which the schema rejects; we read both.
    auto parent = mr.parent();
    if (is_revision_container(parent) || mr.child("w:ins") || mr.child("w:del")) return;
    auto w = parent.insert_child_before(q, mr);
    stamp(d, w);
    w.append_move(mr);
}

void insert_text(Document& d, pugi::xml_node p, int pos, const std::u32string& text, const Json& rpr,
                 const EditOpts& o) {
    if (text.empty()) return;
    int n = vlen(p);
    if (pos < 0 || pos > n) throw std::runtime_error("offset " + std::to_string(pos) + " outside paragraph (length " + std::to_string(n) + ")");
    InsPoint ip = insertion_point(p, pos);
    pugi::xml_node fmt = ip.fmt_run ? ip.fmt_run.child("w:rPr") : pugi::xml_node{};
    bool track = tracking(d, o);
    pugi::xml_node run;
    // Typing inside one's own tracked insertion extends it (Word merges).
    pugi::xml_node own_ins;
    if (track && ip.fmt_run) {
        auto c = container_of(ip.fmt_run);
        if (c && name_is(c, "w:ins") && mine(d, c)) own_ins = c;
    }
    if (own_ins) {
        // insert inside the container, after fmt_run
        run = own_ins.insert_child_after("w:r", ip.fmt_run);
        copy_rpr(run, fmt);
        append_text(run, text);
    } else {
        if (!ip.fmt_run && !rpr.is_object()) {
            // empty paragraph: inherit the paragraph mark's formatting (Word does)
            run = ip.before ? ip.parent.insert_child_before("w:r", ip.before) : ip.parent.append_child("w:r");
            copy_mark_rpr(run, p);
            append_text(run, text);
        } else {
            run = make_run(ip.parent, ip.before, fmt, text);
        }
        if (track) wrap(d, run, "w:ins");
    }
    if (rpr.is_object() && !rpr.empty()) {
        auto r = run.child("w:rPr");
        if (!r) r = run.prepend_child("w:rPr");
        apply_rpr_json(d, r, rpr);
        if (!r.first_child()) run.remove_child(r);
    }
    tidy(p);
}

namespace {

// Delete one run (fully inside the range).
void delete_run(Document& d, pugi::xml_node run, bool track) {
    auto c = container_of(run);
    if (!track) {
        if (c && (name_is(c, "w:del") || name_is(c, "w:moveFrom"))) return;  // already gone
        run.parent().remove_child(run);
        return;
    }
    if (c && (name_is(c, "w:del") || name_is(c, "w:moveFrom"))) return;  // already deleted
    if (c && name_is(c, "w:ins") && mine(d, c)) {
        // deleting your own insertion just removes it
        c.remove_child(run);
        return;
    }
    if (c && name_is(c, "w:moveTo") && mine(d, c)) {
        c.remove_child(run);
        return;
    }
    to_deleted(run);
    // another author's insertion: nest a deletion inside it
    wrap(d, run, "w:del");
}

}  // namespace

void delete_text(Document& d, pugi::xml_node p, int s, int e, const EditOpts& o) {
    int n = vlen(p);
    if (s < 0 || e > n || s > e) throw std::runtime_error("range outside paragraph (length " + std::to_string(n) + ")");
    if (s == e) return;
    bool track = tracking(d, o);
    auto runs = isolate(p, s, e);
    for (auto r : runs) {
        if (name_is(r, "m:oMath") || name_is(r, "m:oMathPara")) {
            if (!track) { r.parent().remove_child(r); continue; }
            // math is deleted by marking its runs
            for (auto mr : xml::descendants(r, "m:r")) wrap_math_run(d, mr, "w:del");
            continue;
        }
        delete_run(d, r, track);
    }
    tidy(p);
}

void replace_text(Document& d, pugi::xml_node p, int s, int e, const std::u32string& text, const Json& rpr,
                  const EditOpts& o) {
    // Word puts the insertion after the deletion. Inherit the formatting of
    // the first replaced character.
    Json fmt = rpr;
    pugi::xml_node first_rpr;
    {
        ParaText pt = analyze(p);
        if (s < (int)pt.vis.size()) first_rpr = pt.atoms[pt.vis[s]].run.child("w:rPr");
    }
    pugi::xml_document keep;
    if (first_rpr) keep.append_copy(first_rpr);
    delete_text(d, p, s, e, o);
    // position after the deleted text: s in visible coordinates (deleted text is invisible)
    int n = vlen(p);
    int pos = std::min(s, n);
    if (text.empty()) return;
    InsPoint ip = insertion_point(p, pos);
    // Word puts the insertion right after the deletion.
    if (tracking(d, o))
        while (ip.before && name_is(ip.before, "w:del")) ip.before = ip.before.next_sibling();
    pugi::xml_node run = make_run(ip.parent, ip.before, keep.first_child(), text);
    if (tracking(d, o)) wrap(d, run, "w:ins");
    if (fmt.is_object() && !fmt.empty()) {
        auto r = run.child("w:rPr");
        if (!r) r = run.prepend_child("w:rPr");
        apply_rpr_json(d, r, fmt);
        if (!r.first_child()) run.remove_child(r);
    }
    tidy(p);
}

// ── find / replace ───────────────────────────────────────────────────────

std::vector<Hit> find(Document& d, const std::string& needle, const FindOpts& f, bool all_stories) {
    std::vector<Hit> hits;
    std::vector<std::string> parts = all_stories ? d.story_parts() : std::vector<std::string>{d.main()};
    std::u32string n32 = util::to_u32(needle);
    std::regex re;
    if (f.regex) {
        auto flags = std::regex::ECMAScript;
        if (!f.match_case) flags |= std::regex::icase;
        re = std::regex(needle, flags);
    }
    auto fold = [](char32_t c) -> char32_t {
        if (c >= 'A' && c <= 'Z') return c + 32;
        if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
        return c;
    };
    auto is_word = [](char32_t c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0; };
    for (auto& part : parts) {
        if (!d.pkg().has(part)) continue;
        for (auto p : paragraphs(d.pkg().xml(part).document_element())) {
            ParaText pt = analyze(p);
            const auto& t = pt.text;
            if (f.regex) {
                std::string s = util::to_utf8(t);
                // map byte offsets to code points
                std::vector<int> cp(s.size() + 1, 0);
                int k = 0;
                for (std::size_t i = 0; i < s.size(); i++) {
                    cp[i] = k;
                    if (i + 1 == s.size() || ((unsigned char)s[i + 1] & 0xC0) != 0x80) k++;
                }
                cp[s.size()] = k;
                for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
                    if (it->length(0) == 0) continue;
                    int a = cp[it->position(0)], b = cp[it->position(0) + it->length(0)];
                    hits.push_back({p, part, a, b});
                    if (f.max >= 0 && (int)hits.size() >= f.max) return hits;
                }
                continue;
            }
            if (n32.empty()) continue;
            for (std::size_t i = 0; i + n32.size() <= t.size(); i++) {
                bool ok = true;
                for (std::size_t j = 0; j < n32.size(); j++) {
                    char32_t a = t[i + j], b = n32[j];
                    if (f.match_case ? a != b : fold(a) != fold(b)) { ok = false; break; }
                }
                if (!ok) continue;
                if (f.whole_word) {
                    if (i > 0 && is_word(t[i - 1])) continue;
                    if (i + n32.size() < t.size() && is_word(t[i + n32.size()])) continue;
                }
                hits.push_back({p, part, (int)i, (int)(i + n32.size())});
                i += n32.size() - 1;
                if (f.max >= 0 && (int)hits.size() >= f.max) return hits;
            }
        }
    }
    return hits;
}

int replace_all(Document& d, const std::string& needle, const std::string& repl, const FindOpts& f,
                const Json& rpr, const EditOpts& o) {
    // group hits by paragraph; replace each paragraph's hits back to front so
    // earlier offsets stay valid
    std::map<pugi::xml_node_struct*, std::vector<Hit>> by;
    std::vector<pugi::xml_node> order;
    for (auto& h : find(d, needle, f)) {
        if (!by.count(h.p.internal_object())) order.push_back(h.p);
        by[h.p.internal_object()].push_back(h);
    }
    int count = 0;
    std::regex re;
    if (f.regex) re = std::regex(needle, f.match_case ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
    for (auto p : order) {
        auto& v = by[p.internal_object()];
        for (auto it = v.rbegin(); it != v.rend(); ++it) {
            std::u32string r32 = util::to_u32(repl);
            if (f.regex) {
                ParaText pt = analyze(p);
                std::string m = util::to_utf8(pt.text.substr(it->start, it->end - it->start));
                r32 = util::to_u32(std::regex_replace(m, re, repl, std::regex_constants::format_first_only));
            }
            replace_text(d, p, it->start, it->end, r32, rpr, o);
            count++;
        }
    }
    return count;
}

// ── paragraphs ───────────────────────────────────────────────────────────

pugi::xml_node split_paragraph(Document& d, pugi::xml_node p, int pos, const EditOpts& o) {
    int n = vlen(p);
    if (pos < 0 || pos > n) throw std::runtime_error("offset outside paragraph");
    split_at(p, pos);
    ParaText pt = analyze(p);
    auto np = p.parent().insert_child_after("w:p", p);
    if (auto ppr = p.child("w:pPr")) {
        auto c = np.append_copy(ppr);
        // the new paragraph's mark is the old one; the old one gets a new mark
        (void)c;
    }
    // move everything from the split point into the new paragraph
    pugi::xml_node first_moved;
    if (pos < n) {
        pugi::xml_node node = pt.atoms[pt.vis[pos]].run;
        while (node.parent() != p) node = node.parent();
        first_moved = node;
    }
    if (first_moved) {
        std::vector<pugi::xml_node> mv;
        for (auto c = first_moved; c; c = c.next_sibling()) mv.push_back(c);
        for (auto c : mv) np.append_move(c);
    }
    // Next paragraph style (Heading -> Normal) when splitting at the end, like Enter.
    if (pos == n) {
        auto sty = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
        if (!sty.empty()) {
            auto styles = d.styles(false);
            if (styles) {
                for (auto s : styles.children("w:style")) {
                    if (xml::attr(s, "w:styleId") != sty) continue;
                    auto next = xml::attr(s.child("w:next"), "w:val");
                    if (!next.empty() && next != sty) {
                        auto nppr = schema::ensure_child(np, "w:pPr");
                        if (next == "Normal") nppr.remove_child("w:pStyle");
                        else xml::set_attr(schema::ensure_child(nppr, "w:pStyle"), "w:val", next);
                        nppr.remove_child("w:numPr");
                        if (!nppr.first_child()) np.remove_child(nppr);
                    }
                    break;
                }
            }
        }
    }
    // the first paragraph's mark is new: tracked as inserted
    if (tracking(d, o)) {
        auto ppr = schema::ensure_child(p, "w:pPr");
        auto rpr = schema::ensure_child(ppr, "w:rPr");
        rpr.remove_child("w:del");
        auto ins = schema::ensure_child(rpr, "w:ins");
        stamp(d, ins);
        // and the new paragraph's mark keeps the original's revision state
        if (auto nrpr = np.child("w:pPr").child("w:rPr")) { (void)nrpr; }
    }
    if (p.attribute("w14:paraId")) {
        auto a = np.prepend_attribute("w14:paraId");
        a = d.new_hex_id().c_str();
        np.insert_attribute_after("w14:textId", np.attribute("w14:paraId")) = "77777777";
    }
    // keep sectPr on the second paragraph (it ends the section)
    if (auto sp = p.child("w:pPr").child("w:sectPr")) {
        p.child("w:pPr").remove_child(sp);
    }
    if (auto np_ppr = np.child("w:pPr"); np_ppr && !p.child("w:pPr").child("w:sectPr")) {
        // np got a copy including sectPr if p had one: correct
    }
    tidy(p);
    tidy(np);
    return np;
}

pugi::xml_node insert_paragraph(Document& d, pugi::xml_node ref, bool after, const std::u32string& text,
                                const std::string& style, const Json& rpr, const EditOpts& o) {
    pugi::xml_node np;
    // inserting after a paragraph that holds a section break would move it; insert before
    // the next block instead, which is equivalent.
    if (after) np = ref.parent().insert_child_after("w:p", ref);
    else np = ref.parent().insert_child_before("w:p", ref);
    if (name_is(ref, "w:p")) {
        if (auto ppr = ref.child("w:pPr")) {
            auto c = np.append_copy(ppr);
            c.remove_child("w:sectPr");
            c.remove_child("w:pPrChange");
            if (auto r = c.child("w:rPr")) {
                for (auto q : {"w:ins", "w:del", "w:moveFrom", "w:moveTo", "w:rPrChange"}) r.remove_child(q);
                if (!r.first_child()) c.remove_child(r);
            }
            if (!c.first_child()) np.remove_child(c);
        }
        // Like pressing Enter at the end: the new paragraph gets the style's
        // "Style for following paragraph" (Heading 1 -> Normal), not a copy.
        std::string sty = xml::attr(ref.child("w:pPr").child("w:pStyle"), "w:val");
        if (after && style.empty() && !sty.empty()) {
            if (auto s = find_style(d, sty)) {
                std::string next = xml::attr(s.child("w:next"), "w:val");
                if (!next.empty() && next != sty) {
                    auto ppr = schema::ensure_child(np, "w:pPr");
                    if (next == "Normal") ppr.remove_child("w:pStyle");
                    else xml::set_attr(schema::ensure_child(ppr, "w:pStyle"), "w:val", next);
                    // direct formatting belonged to the heading, not the next paragraph
                    for (auto q : {"w:numPr", "w:keepNext", "w:outlineLvl", "w:jc", "w:spacing", "w:ind"}) ppr.remove_child(q);
                    if (!ppr.first_child()) np.remove_child(ppr);
                }
            }
        }
    }
    if (!style.empty()) {
        auto ppr = schema::ensure_child(np, "w:pPr");
        std::string sid = ensure_style(d, style);
        if (sid == "Normal") ppr.remove_child("w:pStyle");
        else xml::set_attr(schema::ensure_child(ppr, "w:pStyle"), "w:val", sid);
        ppr.remove_child("w:numPr");
        if (!ppr.first_child()) np.remove_child(ppr);
    }
    auto pid = np.prepend_attribute("w14:paraId");
    pid = d.new_hex_id().c_str();
    np.insert_attribute_after("w14:textId", pid) = "77777777";
    xml::declare(xml::root_of(np), "w14", true);
    std::vector<pugi::xml_node> made{np};
    if (!text.empty()) {
        // split text on \n into multiple paragraphs
        std::vector<std::u32string> lines{U""};
        for (char32_t c : text) {
            if (c == U'\n' || c == U'\r') lines.emplace_back();
            else lines.back().push_back(c);
        }
        pugi::xml_node cur = np;
        for (std::size_t i = 0; i < lines.size(); i++) {
            if (i > 0) {
                auto nx = cur.parent().insert_copy_after(np, cur);
                for (auto c = nx.first_child(); c;) {
                    auto k = c.next_sibling();
                    if (!name_is(c, "w:pPr")) nx.remove_child(c);
                    c = k;
                }
                xml::set_attr(nx, "w14:paraId", d.new_hex_id());
                cur = nx;
                made.push_back(nx);
            }
            if (!lines[i].empty()) {
                auto r = cur.append_child("w:r");
                copy_mark_rpr(r, cur);
                append_text(r, lines[i]);
                if (rpr.is_object() && !rpr.empty()) {
                    auto rp = r.child("w:rPr");
                    if (!rp) rp = r.prepend_child("w:rPr");
                    apply_rpr_json(d, rp, rpr);
                }
            }
        }
    }
    if (tracking(d, o)) {
        // Word marks each inserted paragraph mark and wraps the runs.
        for (auto p : made) {
            auto ppr = schema::ensure_child(p, "w:pPr");
            auto mrpr = schema::ensure_child(ppr, "w:rPr");
            stamp(d, schema::ensure_child(mrpr, "w:ins"));
            std::vector<pugi::xml_node> runs;
            for (auto c : p.children("w:r")) runs.push_back(c);
            for (auto r : runs) wrap(d, r, "w:ins");
            tidy(p);
        }
    }
    return np;
}

int move_paragraphs(Document& d, const std::vector<pugi::xml_node>& ps_in, pugi::xml_node tgt, bool after,
                    const EditOpts& o) {
    std::vector<pugi::xml_node> ps;
    for (auto p : ps_in)
        if (p != tgt) ps.push_back(p);
    if (ps.empty()) return 0;
    if (!tracking(d, o)) {
        pugi::xml_node anchor = tgt;
        for (auto p : ps) {
            if (after) { anchor.parent().insert_move_after(p, anchor); anchor = p; }
            else tgt.parent().insert_move_before(p, tgt);
        }
        return (int)ps.size();
    }
    // A tracked move is one named move: moveFromRange around the originals,
    // moveToRange around the copies, sharing a name (Word's "move123456789").
    std::string name = "move" + std::to_string(100000000 + util::random_u32() % 900000000);
    auto range = [&](pugi::xml_node first_p, pugi::xml_node last_p, const char* qs, const char* qe) {
        auto s = first_p.insert_child_after(qs, first_p.child("w:pPr"));
        if (!first_p.child("w:pPr")) {
            first_p.remove_child(s);
            s = first_p.prepend_child(qs);
        }
        stamp(d, s);
        xml::set_attr(s, "w:name", name);
        // Word requires a date on move ranges
        if (!s.attribute("w:date")) xml::set_attr(s, "w:date", d.date());
        auto e = last_p.append_child(qe);
        xml::set_attr(e, "w:id", xml::attr(s, "w:id"));
    };
    std::vector<pugi::xml_node> copies;
    pugi::xml_node anchor = tgt;
    for (auto p : ps) {
        pugi::xml_document copy;
        copy.append_copy(p);
        auto np = after ? anchor.parent().insert_copy_after(copy.first_child(), anchor)
                        : tgt.parent().insert_copy_before(copy.first_child(), tgt);
        xml::set_attr(np, "w14:paraId", d.new_hex_id());
        mark_paragraph_inserted(d, np, "w:moveTo");
        copies.push_back(np);
        if (after) anchor = np;
    }
    // the originals: their text and marks become w:moveFrom (own insertions
    // just go, like deleting your own inserted text)
    for (auto p : ps) {
        std::function<void(pugi::xml_node)> wrap = [&](pugi::xml_node n) {
            std::vector<pugi::xml_node> run;
            auto flush = [&]() {
                if (run.empty()) return;
                auto mf = n.insert_child_before("w:moveFrom", run.front());
                stamp(d, mf);
                for (auto r : run) mf.append_move(r);
                run.clear();
            };
            for (auto c = n.first_child(); c;) {
                auto nx = c.next_sibling();
                std::string_view q = c.name();
                if (q == "w:r") run.push_back(c);
                else if (q == "w:ins") {
                    flush();
                    if (mine(d, c)) n.remove_child(c);  // own insertion: nothing to move from
                    else { wrap(c); }
                } else if (q == "w:del" || q == "w:moveFrom") {
                    flush();  // already gone; stays as it is
                } else {
                    flush();
                    if (q == "w:hyperlink" || q == "w:fldSimple" || q == "w:smartTag" || q == "w:customXml" ||
                        q == "w:sdt" || q == "w:sdtContent")
                        wrap(c);
                }
                c = nx;
            }
            flush();
        };
        wrap(p);
        auto rpr = schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:rPr");
        if (!rpr.child("w:moveFrom") && !rpr.child("w:del")) stamp(d, schema::ensure_child(rpr, "w:moveFrom"));
    }
    range(ps.front(), ps.back(), "w:moveFromRangeStart", "w:moveFromRangeEnd");
    range(copies.front(), copies.back(), "w:moveToRangeStart", "w:moveToRangeEnd");
    return (int)ps.size();
}

void delete_paragraph(Document& d, pugi::xml_node p, const EditOpts& o) {
    if (!tracking(d, o)) {
        auto parent = p.parent();
        // A cell or the body must keep a paragraph.
        int count = 0;
        for (auto c : parent.children()) if (name_is(c, "w:p") || name_is(c, "w:tbl")) count++;
        if (count <= 1 || (name_is(parent, "w:body") && count <= 1)) {
            for (auto c = p.first_child(); c;) {
                auto nx = c.next_sibling();
                if (!name_is(c, "w:pPr")) p.remove_child(c);
                c = nx;
            }
            return;
        }
        // keep a section break: move sectPr to the previous paragraph
        if (auto sp = p.child("w:pPr").child("w:sectPr")) {
            auto prev = p.previous_sibling("w:p");
            if (prev) {
                auto pp = schema::ensure_child(prev, "w:pPr");
                if (!pp.child("w:sectPr")) schema::place_child(pp, pp.append_move(sp));
            }
        }
        parent.remove_child(p);
        return;
    }
    int n = vlen(p);
    if (n > 0) delete_text(d, p, 0, n, o);
    // the last paragraph of a container can't lose its mark
    pugi::xml_node next = p.next_sibling();
    while (next && !name_is(next, "w:p") && !name_is(next, "w:tbl") && !name_is(next, "w:sdt")) next = next.next_sibling();
    if (!next) return;
    auto ppr = schema::ensure_child(p, "w:pPr");
    auto mrpr = schema::ensure_child(ppr, "w:rPr");
    if (auto ins = mrpr.child("w:ins"); ins && mine(d, ins)) {
        // own inserted paragraph: remove entirely
        p.parent().remove_child(p);
        return;
    }
    if (!mrpr.child("w:del")) stamp(d, schema::ensure_child(mrpr, "w:del"));
}

void merge_with_next(Document& d, pugi::xml_node p, const EditOpts& o) {
    auto next = p.next_sibling("w:p");
    if (!next) throw std::runtime_error("no following paragraph to merge with");
    if (tracking(d, o)) {
        auto ppr = schema::ensure_child(p, "w:pPr");
        auto mrpr = schema::ensure_child(ppr, "w:rPr");
        if (!mrpr.child("w:del")) stamp(d, schema::ensure_child(mrpr, "w:del"));
        return;
    }
    // untracked: move next's content into p; p takes next's mark properties (Word keeps the
    // second paragraph's properties when deleting the mark)
    std::vector<pugi::xml_node> mv;
    for (auto c : next.children()) if (!name_is(c, "w:pPr")) mv.push_back(c);
    for (auto c : mv) p.append_move(c);
    p.remove_child("w:pPr");
    if (auto np = next.child("w:pPr")) p.prepend_copy(np);
    p.parent().remove_child(next);
    tidy(p);
}

// ── formatting ───────────────────────────────────────────────────────────

namespace {
// Record the pre-change rPr in rPrChange (Word keeps the first original).
void record_rpr_change(Document& d, pugi::xml_node rpr) {
    if (rpr.child("w:rPrChange")) return;
    pugi::xml_document old;
    auto o = old.append_child("w:rPr");
    for (auto c : rpr.children()) {
        std::string_view q = c.name();
        if (q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo" || q == "w:rPrChange") continue;
        o.append_copy(c);
    }
    auto ch = schema::ensure_child(rpr, "w:rPrChange");
    stamp(d, ch);
    ch.append_copy(o);
}
}  // namespace

void format_runs(Document& d, pugi::xml_node p, int s, int e, const Json& props, const EditOpts& o) {
    if (s >= e) return;
    bool track = tracking(d, o);
    auto runs = isolate(p, s, e);
    for (auto r : runs) {
        if (!name_is(r, "w:r")) continue;
        auto rpr = r.child("w:rPr");
        if (!rpr) rpr = r.prepend_child("w:rPr");
        // inserted text formatted by its own author is just formatted (Word)
        auto c = container_of(r);
        bool own_ins = c && name_is(c, "w:ins") && mine(d, c);
        if (track && !own_ins) record_rpr_change(d, rpr);
        apply_rpr_json(d, rpr, props);
        if (!rpr.first_child()) r.remove_child(rpr);
    }
    tidy(p);
}

void format_paragraph(Document& d, pugi::xml_node p, const Json& props, const EditOpts& o) {
    auto ppr = schema::ensure_child(p, "w:pPr");
    if (tracking(d, o) && !ppr.child("w:pPrChange")) {
        pugi::xml_document old;
        auto op = old.append_child("w:pPr");
        for (auto c : ppr.children()) {
            std::string_view q = c.name();
            // pPrChange holds CT_PPrBase: no rPr, sectPr or pPrChange
            if (q == "w:rPr" || q == "w:sectPr" || q == "w:pPrChange") continue;
            op.append_copy(c);
        }
        apply_ppr_json(d, ppr, props);
        auto ch = schema::ensure_child(ppr, "w:pPrChange");
        stamp(d, ch);
        ch.append_copy(op);
    } else {
        apply_ppr_json(d, ppr, props);
    }
    if (!ppr.first_child()) p.remove_child(ppr);
}

}  // namespace docx::wml

#include "wml/text.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/document.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool name_is(pugi::xml_node n, const char* q) { return std::string_view(n.name()) == q; }

Rev rev_of(pugi::xml_node n) {
    std::string_view q = n.name();
    if (q == "w:ins") return Rev::Ins;
    if (q == "w:del") return Rev::Del;
    if (q == "w:moveFrom") return Rev::MoveFrom;
    if (q == "w:moveTo") return Rev::MoveTo;
    return Rev::None;
}

struct Walker {
    std::vector<Atom>& out;
    int fld_depth = 0;          // complex field nesting
    std::vector<bool> in_code;  // per nesting: still in instruction part

    bool code() const { return !in_code.empty() && in_code.back(); }

    void run(pugi::xml_node r, Rev rev, pugi::xml_node rn, pugi::xml_node ins) {
        // A run's own rPr can mark it deleted/inserted too (paragraph marks only
        // use pPr/rPr; run rPr ins/del is not used by Word for text).
        for (auto c : r.children()) {
            if (c.type() != pugi::node_element) continue;
            std::string_view q = c.name();
            auto push = [&](char32_t ch, int off, bool obj) {
                Atom a;
                a.run = r; a.node = c; a.off = off; a.ch = ch; a.rev = rev; a.rev_node = rn; a.ins_node = ins;
                a.field_code = code(); a.object = obj;
                out.push_back(a);
            };
            if (q == "w:t" || q == "w:delText" || q == "w:instrText" || q == "w:delInstrText") {
                auto u = util::to_u32(c.text().get());
                bool is_code = q == "w:instrText" || q == "w:delInstrText";
                for (int i = 0; i < (int)u.size(); i++) {
                    Atom a;
                    a.run = r; a.node = c; a.off = i; a.ch = u[i]; a.rev = rev; a.rev_node = rn; a.ins_node = ins;
                    a.field_code = is_code || code();
                    out.push_back(a);
                }
            } else if (q == "w:tab" || q == "w:ptab") push(U'\t', 0, false);
            else if (q == "w:br") {
                std::string t = xml::attr(c, "w:type", "textWrapping");
                push(t == "page" || t == "column" ? U'\f' : U'\v', 0, false);
            } else if (q == "w:cr") push(U'\v', 0, false);
            else if (q == "w:noBreakHyphen") push(U'\u2011', 0, false);
            else if (q == "w:softHyphen") push(U'\u00AD', 0, false);
            else if (q == "w:sym") {
                std::string chs = xml::attr(c, "w:char");
                char32_t ch = kObj;
                if (!chs.empty()) {
                    unsigned long v = std::strtoul(chs.c_str(), nullptr, 16);
                    if (v >= 0xF000 && v <= 0xF0FF) v -= 0xF000;
                    // anything that isn't a scalar value shows as an object
                    ch = (v && v <= 0x10FFFF && (v < 0xD800 || v > 0xDFFF)) ? char32_t(v) : kObj;
                }
                push(ch, 0, false);
            } else if (q == "w:fldChar") {
                std::string t = xml::attr(c, "w:fldCharType");
                if (t == "begin") { fld_depth++; in_code.push_back(true); }
                else if (t == "separate") { if (!in_code.empty()) in_code.back() = false; }
                else if (t == "end") { if (fld_depth) fld_depth--; if (!in_code.empty()) in_code.pop_back(); }
            } else if (q == "w:drawing" || q == "w:pict" || q == "w:object" || q == "w:footnoteReference" ||
                       q == "w:endnoteReference" || q == "mc:AlternateContent" || q == "w:embedded") {
                push(kObj, 0, true);
            } else if (q == "w:commentReference" || q == "w:annotationRef" || q == "w:footnoteRef" ||
                       q == "w:endnoteRef" || q == "w:separator" || q == "w:continuationSeparator" ||
                       q == "w:lastRenderedPageBreak" || q == "w:rPr" || q == "w:dayShort" ||
                       q == "w:monthShort" || q == "w:yearShort" || q == "w:dayLong" || q == "w:monthLong" ||
                       q == "w:yearLong" || q == "w:pgNum") {
                // no visible character of its own
            }
        }
    }

    void walk(pugi::xml_node n, Rev rev, pugi::xml_node rn, pugi::xml_node ins) {
        for (auto c : n.children()) {
            if (c.type() != pugi::node_element) continue;
            std::string_view q = c.name();
            if (q == "w:pPr") continue;
            if (q == "w:r") { run(c, rev, rn, ins); continue; }
            Rev r2 = rev_of(c);
            if (r2 != Rev::None) {
                pugi::xml_node ins2 = r2 == Rev::Ins ? c : ins;
                // a deletion nested in an insertion reads as deleted
                walk(c, r2 == Rev::Ins && rev == Rev::Del ? Rev::Del : r2, c, ins2);
                continue;
            }
            if (q == "m:oMath" || q == "m:oMathPara") {
                // an equation whose runs were all removed (rejected insertion) has no content
                bool has_text = false;
                for (auto t : xml::descendants(c, "m:t")) if (*t.text().get()) has_text = true;
                if (!has_text) continue;
                Atom a;
                a.run = c; a.node = c; a.ch = kObj; a.rev = rev; a.rev_node = rn; a.ins_node = ins; a.object = true;
                out.push_back(a);
                continue;
            }
            if (q == "mc:AlternateContent") {
                // run-level AC: take the Choice when present (Word reads it)
                auto ch = c.child("mc:Choice");
                if (!ch) ch = c.child("mc:Fallback");
                if (ch) walk(ch, rev, rn, ins);
                continue;
            }
            if (q == "w:hyperlink" || q == "w:smartTag" || q == "w:customXml" || q == "w:fldSimple" ||
                q == "w:sdt" || q == "w:sdtContent" || q == "w:dir" || q == "w:bdo" || q == "w:ruby" ||
                q == "w:rubyBase") {
                if (q == "w:sdt") {
                    if (auto sc = c.child("w:sdtContent")) walk(sc, rev, rn, ins);
                } else {
                    walk(c, rev, rn, ins);
                }
            }
        }
    }
};

}  // namespace

std::string ParaText::utf8() const { return util::to_utf8(text); }

ParaText analyze(pugi::xml_node p) {
    ParaText pt;
    Walker w{pt.atoms, 0, {}};
    w.walk(p, Rev::None, {}, {});
    for (int i = 0; i < (int)pt.atoms.size(); i++) {
        auto& a = pt.atoms[i];
        if (a.field_code || a.rev == Rev::Del || a.rev == Rev::MoveFrom) continue;
        pt.vis.push_back(i);
        pt.text.push_back(a.ch);
    }
    return pt;
}

std::u32string original_text(pugi::xml_node p) {
    ParaText pt;
    Walker w{pt.atoms, 0, {}};
    w.walk(p, Rev::None, {}, {});
    std::u32string s;
    for (auto& a : pt.atoms) {
        if (a.field_code) continue;
        if (a.rev == Rev::Ins || a.rev == Rev::MoveTo) continue;
        if (a.rev == Rev::Del && a.ins_node) continue;  // inserted then deleted: never existed
        s.push_back(a.ch);
    }
    return s;
}

namespace {
void collect_paras(pugi::xml_node n, std::vector<pugi::xml_node>& out, bool tb) {
    for (auto c : n.children()) {
        if (c.type() != pugi::node_element) continue;
        std::string_view q = c.name();
        if (q == "mc:Fallback") continue;  // duplicate content of the Choice
        if (q == "w:p") {
            out.push_back(c);
            if (tb) collect_paras(c, out, tb);  // text boxes inside runs
            continue;
        }
        if (!tb && (q == "w:txbxContent" || q == "w:drawing" || q == "w:pict")) continue;
        collect_paras(c, out, tb);
    }
}
void collect_blocks(pugi::xml_node n, std::vector<pugi::xml_node>& out) {
    for (auto c : n.children()) {
        if (c.type() != pugi::node_element) continue;
        std::string_view q = c.name();
        if (q == "w:p" || q == "w:tbl") out.push_back(c);
        else if (q == "w:sdt") { if (auto sc = c.child("w:sdtContent")) collect_blocks(sc, out); }
        else if (q == "w:customXml" || q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo")
            collect_blocks(c, out);
    }
}
}  // namespace

std::vector<pugi::xml_node> paragraphs(pugi::xml_node root, bool tb) {
    std::vector<pugi::xml_node> v;
    collect_paras(root, v, tb);
    return v;
}

std::vector<pugi::xml_node> blocks(pugi::xml_node container) {
    std::vector<pugi::xml_node> v;
    collect_blocks(container, v);
    return v;
}

std::string para_id(pugi::xml_node p) { return xml::attr(p, "w14:paraId"); }

void assign_missing_para_ids(Document& d, pugi::xml_node root, const std::string& part) {
    bool any = false;
    for (auto p : paragraphs(root)) {
        if (!p.attribute("w14:paraId")) { any = true; break; }
    }
    if (!any) return;
    d.ensure_para_ids(root);
    auto r = xml::root_of(root);
    xml::declare(r, "w14", true);
}

ParaRef find_para(Document& d, const std::string& ref) {
    std::string part = d.main();
    std::string key = ref;
    if (auto h = ref.find('#'); h != std::string::npos) {
        part = ref.substr(0, h);
        key = ref.substr(h + 1);
        if (!part.empty() && part[0] != '/') part = "/" + part;
    }
    if (!d.pkg().has(part)) throw std::runtime_error("no such part " + part);
    auto root = d.pkg().xml(part).document_element();
    auto ps = paragraphs(root);
    bool numeric = !key.empty() && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (numeric && key.size() < 8) {
        int i = std::stoi(key);
        // index over top-level story paragraphs (not text boxes)
        auto top = paragraphs(root, false);
        if (i >= 1 && i <= (int)top.size()) return {top[i - 1], part};
    }
    for (auto p : ps)
        if (util::iequals(para_id(p), key)) return {p, part};
    // search every story
    if (ref.find('#') == std::string::npos) {
        for (auto& sp : d.story_parts()) {
            if (sp == part || !d.pkg().has(sp)) continue;
            for (auto p : paragraphs(d.pkg().xml(sp).document_element()))
                if (util::iequals(para_id(p), key)) return {p, sp};
        }
    }
    throw std::runtime_error("paragraph '" + ref + "' not found (use the id from read_document)");
}

// ── run surgery ─────────────────────────────────────────────────────────

namespace {

// Split w:t at code point offset; returns the second w:t (in a new run).
pugi::xml_node split_run(pugi::xml_node run, pugi::xml_node at_child, int off) {
    // New run after `run` with the same rPr and attributes, taking every child
    // from `at_child` (split inside it when off > 0).
    auto parent = run.parent();
    auto nr = parent.insert_copy_after(run, run);
    // remove all content children from the copy
    for (auto c = nr.first_child(); c;) {
        auto nx = c.next_sibling();
        if (!name_is(c, "w:rPr")) nr.remove_child(c);
        c = nx;
    }
    std::vector<pugi::xml_node> move;
    bool hit = false;
    for (auto c : run.children()) {
        if (c == at_child) hit = true;
        if (hit && !name_is(c, "w:rPr")) move.push_back(c);
    }
    if (off > 0) {
        // at_child is a text element: split its text
        auto u = util::to_u32(at_child.text().get());
        std::u32string a = u.substr(0, off), b = u.substr(off);
        auto tail = nr.append_copy(at_child);
        at_child.text().set(util::to_utf8(a).c_str());
        tail.text().set(util::to_utf8(b).c_str());
        auto fix = [](pugi::xml_node t) {
            std::string s = t.text().get();
            bool sp = !s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find("  ") != std::string::npos ||
                                     s.find('\t') != std::string::npos);
            if (sp) xml::set_attr(t, "xml:space", "preserve");
        };
        fix(at_child);
        fix(tail);
        move.erase(move.begin());  // at_child stays (holds the head)
    }
    for (auto c : move) nr.append_move(c);
    return nr;
}

// Split the revision container `rc` (w:ins/del/...) so that `run` begins a new one.
void split_container_before(pugi::xml_node rc, pugi::xml_node first_child) {
    if (first_child == rc.first_child()) return;
    auto copy = rc.parent().insert_copy_after(rc, rc);
    for (auto c = copy.first_child(); c;) {
        auto nx = c.next_sibling();
        copy.remove_child(c);
        c = nx;
    }
    std::vector<pugi::xml_node> move;
    bool hit = false;
    for (auto c : rc.children()) {
        if (c == first_child) hit = true;
        if (hit) move.push_back(c);
    }
    for (auto c : move) copy.append_move(c);
}

}  // namespace

void split_at(pugi::xml_node p, int pos) {
    ParaText pt = analyze(p);
    if (pos <= 0 || pos >= (int)pt.vis.size()) {
        // still split containers at the boundary for pos == len handled by caller
        return;
    }
    const Atom& a = pt.atoms[pt.vis[pos]];
    pugi::xml_node run = a.run;
    if (!name_is(run, "w:r")) return;  // math object: atomic
    // already at run start?
    bool at_start = true;
    for (auto c : run.children()) {
        if (c == a.node) break;
        if (c.type() == pugi::node_element && !name_is(c, "w:rPr") && !name_is(c, "w:lastRenderedPageBreak")) { at_start = false; break; }
    }
    if (a.off > 0) at_start = false;
    pugi::xml_node newrun = run;
    if (!at_start) newrun = split_run(run, a.node, a.off);
    // split enclosing revision containers / hyperlinks? Only revision containers
    // need splitting; the caller wraps runs individually.
    for (auto anc = newrun.parent(); anc && !name_is(anc, "w:p"); anc = anc.parent()) {
        if (is_revision_container(anc)) {
            // find the direct child of anc containing newrun
            pugi::xml_node child = newrun;
            while (child.parent() != anc) child = child.parent();
            split_container_before(anc, child);
            break;
        }
    }
}

std::vector<pugi::xml_node> isolate(pugi::xml_node p, int s, int e) {
    split_at(p, s);
    split_at(p, e);
    ParaText pt = analyze(p);
    std::vector<pugi::xml_node> runs;
    for (int i = s; i < e && i < (int)pt.vis.size(); i++) {
        auto r = pt.atoms[pt.vis[i]].run;
        if (runs.empty() || runs.back() != r) runs.push_back(r);
    }
    return runs;
}

void append_text(pugi::xml_node run, const std::u32string& text, bool deleted) {
    std::u32string buf;
    auto flush = [&]() {
        if (buf.empty()) return;
        auto t = run.append_child(deleted ? "w:delText" : "w:t");
        std::string s = util::xml_safe(util::to_utf8(buf));
        if (!s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find("  ") != std::string::npos))
            xml::set_attr(t, "xml:space", "preserve");
        t.text().set(s.c_str());
        buf.clear();
    };
    for (char32_t c : text) {
        switch (c) {
            case U'\t': flush(); run.append_child("w:tab"); break;
            case U'\v': case U'\n': flush(); run.append_child("w:br"); break;
            case U'\f': flush(); xml::set_attr(run.append_child("w:br"), "w:type", "page"); break;
            case U'\r': break;
            case U'\u2011': flush(); run.append_child("w:noBreakHyphen"); break;
            case U'\u00AD': flush(); run.append_child("w:softHyphen"); break;
            default: buf.push_back(c);
        }
    }
    flush();
}

void copy_rpr(pugi::xml_node dst_run, pugi::xml_node src_rpr) {
    if (!src_rpr) return;
    auto r = dst_run.prepend_copy(src_rpr);
    for (auto q : {"w:ins", "w:del", "w:moveFrom", "w:moveTo", "w:rPrChange"}) r.remove_child(q);
    if (!r.first_child()) dst_run.remove_child(r);
}

void copy_mark_rpr(pugi::xml_node dst_run, pugi::xml_node p) {
    auto mark = p.child("w:pPr").child("w:rPr");
    if (!mark) return;
    auto r = dst_run.prepend_child("w:rPr");
    for (auto c : mark.children()) {
        std::string_view q = c.name();
        if (q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo" || q == "w:rPrChange") continue;
        r.append_copy(c);
    }
    if (!r.first_child()) dst_run.remove_child(r);
}

pugi::xml_node make_run(pugi::xml_node parent, pugi::xml_node before, pugi::xml_node rpr,
                        const std::u32string& text, bool deleted) {
    auto r = before ? parent.insert_child_before("w:r", before) : parent.append_child("w:r");
    copy_rpr(r, rpr);
    append_text(r, text, deleted);
    return r;
}

bool is_revision_container(pugi::xml_node n) {
    std::string_view q = n.name();
    return q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo";
}

namespace {
bool same_rpr(pugi::xml_node a, pugi::xml_node b) {
    auto ra = a.child("w:rPr"), rb = b.child("w:rPr");
    std::string sa = ra ? xml::to_string(ra) : "", sb = rb ? xml::to_string(rb) : "";
    return sa == sb;
}
bool plain_text_run(pugi::xml_node r) {
    for (auto c : r.children()) {
        if (c.type() != pugi::node_element) continue;
        std::string_view q = c.name();
        if (q != "w:rPr" && q != "w:t" && q != "w:delText") return false;
    }
    return true;
}
bool same_container(pugi::xml_node a, pugi::xml_node b) {
    return std::string_view(a.name()) == b.name() && xml::attr(a, "w:author") == xml::attr(b, "w:author") &&
           xml::attr(a, "w:date") == xml::attr(b, "w:date");
}
void tidy_level(pugi::xml_node n) {
    for (auto c = n.first_child(); c;) {
        auto nx = c.next_sibling();
        if (!nx) break;
        if (name_is(c, "w:r") && name_is(nx, "w:r") && plain_text_run(c) && plain_text_run(nx) && same_rpr(c, nx)) {
            // merge texts of the same kind
            auto lt = c.last_child();
            auto ft = nx.child("w:t") ? nx.child("w:t") : nx.child("w:delText");
            if (lt && ft && std::string_view(lt.name()) == ft.name() && nx.find_child([](pugi::xml_node k) {
                    return k.type() == pugi::node_element && !name_is(k, "w:rPr") && !name_is(k, "w:t") && !name_is(k, "w:delText");
                }) == pugi::xml_node()) {
                std::vector<pugi::xml_node> texts;
                for (auto k : nx.children()) if (!name_is(k, "w:rPr")) texts.push_back(k);
                bool ok = true;
                for (auto k : texts) if (std::string_view(k.name()) != lt.name()) ok = false;
                if (ok) {
                    std::string s = lt.text().get();
                    for (auto k : texts) s += k.text().get();
                    lt.text().set(s.c_str());
                    if (!s.empty() && (s.front() == ' ' || s.back() == ' ' || s.find("  ") != std::string::npos))
                        xml::set_attr(lt, "xml:space", "preserve");
                    n.remove_child(nx);
                    continue;  // try merging the next one into c
                }
            }
        }
        if (is_revision_container(c) && is_revision_container(nx) && same_container(c, nx)) {
            for (auto k = nx.first_child(); k;) {
                auto kn = k.next_sibling();
                c.append_move(k);
                k = kn;
            }
            n.remove_child(nx);
            continue;
        }
        c = nx;
    }
    for (auto c : n.children())
        if (is_revision_container(c) || name_is(c, "w:hyperlink") || name_is(c, "w:smartTag")) tidy_level(c);
}
}  // namespace

void tidy(pugi::xml_node p) {
    // drop empty runs / containers left by edits
    std::function<void(pugi::xml_node)> prune = [&](pugi::xml_node n) {
        for (auto c = n.first_child(); c;) {
            auto nx = c.next_sibling();
            if (is_revision_container(c)) {
                prune(c);
                if (!c.find_child([](pugi::xml_node k) { return k.type() == pugi::node_element; })) n.remove_child(c);
            } else if (name_is(c, "w:r")) {
                for (auto k = c.first_child(); k;) {
                    auto kn = k.next_sibling();
                    if ((name_is(k, "w:t") || name_is(k, "w:delText")) && !*k.text().get()) c.remove_child(k);
                    k = kn;
                }
                bool content = false;
                for (auto k : c.children())
                    if (k.type() == pugi::node_element && !name_is(k, "w:rPr")) content = true;
                if (!content) n.remove_child(c);
            } else if (name_is(c, "w:hyperlink") || name_is(c, "w:smartTag")) {
                prune(c);
            }
            c = nx;
        }
    };
    prune(p);
    tidy_level(p);
}

}  // namespace docx::wml

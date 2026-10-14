// Accept / reject tracked changes (ECMA-376 Part 1 §17.13). Covers run
// insert/delete/move, paragraph-mark insert/delete/move, property changes
// (rPr/pPr/sectPr/tblPr/trPr/tcPr/tblGrid/tblPrEx/numbering), table row and
// cell insert/delete/merge, math revisions, field code text and custom XML
// / content control ranges.
#include <algorithm>
#include <functional>
#include <set>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/edit.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool name_is(pugi::xml_node n, std::string_view q) { return n && n.type() == pugi::node_element && q == n.name(); }

struct Ctx {
    Document& d;
    const RevFilter& f;
    bool accept;
    int count = 0;
    std::set<std::string> handled_ids;

    bool want(pugi::xml_node rev) {
        if (!f.author.empty() && xml::attr(rev, "w:author") != f.author) return false;
        if (!f.ids.empty()) {
            std::string id = xml::attr(rev, "w:id");
            if (std::find(f.ids.begin(), f.ids.end(), id) == f.ids.end()) return false;
        }
        return true;
    }
    void hit(pugi::xml_node rev) {
        auto id = xml::attr(rev, "w:id");
        if (handled_ids.insert(id).second || id.empty()) count++;
    }
};

const std::set<std::string>& prop_changes() {
    static const std::set<std::string> s = {"w:rPrChange", "w:pPrChange", "w:sectPrChange", "w:tblPrChange",
                                            "w:trPrChange", "w:tcPrChange", "w:tblGridChange", "w:tblPrExChange"};
    return s;
}

void unwrap(pugi::xml_node n) {
    auto parent = n.parent();
    for (auto c = n.first_child(); c;) {
        auto nx = c.next_sibling();
        parent.insert_move_before(c, n);
        c = nx;
    }
    parent.remove_child(n);
}

void live_text(pugi::xml_node n) {
    for (auto t : xml::descendants(n, "w:delText")) t.set_name("w:t");
    for (auto t : xml::descendants(n, "w:delInstrText")) t.set_name("w:instrText");
}

// Remove a field's runs when deleting would leave dangling fldChars: handled
// implicitly because deleted runs carry their fldChars with them.

// ── run-level containers ─────────────────────────────────────────────────

// keep = content survives (accept ins / reject del); else content goes.
void resolve_container(Ctx& c, pugi::xml_node rev, bool keep) {
    c.hit(rev);
    if (keep) {
        live_text(rev);
        unwrap(rev);
    } else {
        rev.parent().remove_child(rev);
    }
}

// Move ranges (moveFromRangeStart/End, moveToRangeStart/End) are dropped once
// their moves are resolved.
void drop_move_ranges(pugi::xml_node root) {
    for (auto q : {"w:moveFromRangeStart", "w:moveFromRangeEnd", "w:moveToRangeStart", "w:moveToRangeEnd"})
        for (auto n : xml::descendants(root, q)) n.parent().remove_child(n);
}

bool any_left(pugi::xml_node root, std::initializer_list<const char*> qs) {
    for (auto q : qs) if (xml::first_descendant(root, q)) return true;
    return false;
}

// Removing rows, tables and paragraph marks can empty a container Word needs
// a paragraph in: the body, a cell (which must also end with one), a
// comment, note, header or footer, a text box.
void ensure_block_paragraphs(pugi::xml_node root) {
    auto needs = [](pugi::xml_node n) {
        std::string_view q = n.name();
        // w:footnote in settings' footnotePr is only an id reference
        if (q == "w:footnote" || q == "w:endnote")
            return std::string_view(n.parent().name()) == (q == "w:footnote" ? "w:footnotes" : "w:endnotes");
        return q == "w:body" || q == "w:tc" || q == "w:comment" || q == "w:hdr" || q == "w:ftr" || q == "w:txbxContent";
    };
    std::vector<pugi::xml_node> conts;
    if (needs(root)) conts.push_back(root);
    for (auto n : root.select_nodes(".//*"))
        if (needs(n.node())) conts.push_back(n.node());
    for (auto c : conts) {
        pugi::xml_node last;
        for (auto k : c.children()) {
            std::string_view q = k.name();
            if (q == "w:p" || q == "w:tbl" || q == "w:sdt" || q == "w:customXml") last = k;
        }
        bool cell = name_is(c, "w:tc");
        if (last && (!cell || name_is(last, "w:p"))) continue;
        // body: before the final sectPr; others: at the end
        if (auto sp = c.child("w:sectPr"); sp && name_is(c, "w:body")) c.insert_child_before("w:p", sp);
        else c.append_child("w:p");
    }
}

// ── paragraph marks ──────────────────────────────────────────────────────

pugi::xml_node mark_rev(pugi::xml_node p, const char* q) { return p.child("w:pPr").child("w:rPr").child(q); }

void drop_mark_rev(pugi::xml_node p, const char* q) {
    auto rpr = p.child("w:pPr").child("w:rPr");
    if (!rpr) return;
    rpr.remove_child(q);
    if (!rpr.first_child()) p.child("w:pPr").remove_child(rpr);
    if (auto ppr = p.child("w:pPr"); ppr && !ppr.first_child() && !ppr.first_attribute()) p.remove_child(ppr);
}

// Next block sibling (paragraph or table) at the same level.
pugi::xml_node next_block(pugi::xml_node n) {
    for (auto s = n.next_sibling(); s; s = s.next_sibling()) {
        std::string_view q = s.name();
        if (q == "w:p" || q == "w:tbl" || q == "w:sdt" || q == "w:customXml") return s;
    }
    return {};
}

// Remove a paragraph mark: join p with the following paragraph. The joined
// paragraph keeps the FOLLOWING paragraph's properties (Word: the mark that
// survives owns the formatting), unless the following block is a table or
// there is none, in which case p keeps its content as is.
void remove_mark(pugi::xml_node p) {
    auto nb = next_block(p);
    if (!nb || !name_is(nb, "w:p")) {
        // Before a table / at the end of a cell or story the mark can't be removed;
        // Word drops an empty paragraph there, else keeps it.
        bool empty = true;
        for (auto c : p.children()) {
            std::string_view q = c.name();
            // range markers carry no content (bookmarks, move/comment ranges)
            bool marker = q == "w:pPr" || q == "w:bookmarkStart" || q == "w:bookmarkEnd" ||
                          q == "w:moveFromRangeStart" || q == "w:moveFromRangeEnd" || q == "w:moveToRangeStart" ||
                          q == "w:moveToRangeEnd" || q == "w:commentRangeStart" || q == "w:commentRangeEnd" ||
                          q == "w:proofErr" || q == "w:permStart" || q == "w:permEnd";
            if (c.type() == pugi::node_element && !marker) empty = false;
        }
        int blocks_in_parent = 0;
        for (auto c : p.parent().children())
            if (name_is(c, "w:p") || name_is(c, "w:tbl")) blocks_in_parent++;
        bool sect = p.child("w:pPr").child("w:sectPr");
        // the only paragraph of a block content control: the control goes with it
        // (its own container then keeps whatever it needs)
        auto sc = p.parent();
        if (empty && blocks_in_parent == 1 && !sect && name_is(sc, "w:sdtContent") && name_is(sc.parent(), "w:sdt")) {
            auto sdt = sc.parent();
            sdt.parent().remove_child(sdt);
            return;
        }
        // the container must keep at least one paragraph
        if (empty && blocks_in_parent > 1 && !sect) {
            p.parent().remove_child(p);
        }
        return;
    }
    // move p's inline content to the front of nb
    std::vector<pugi::xml_node> mv;
    for (auto c : p.children()) if (!name_is(c, "w:pPr")) mv.push_back(c);
    pugi::xml_node first = nb.child("w:pPr") ? nb.child("w:pPr").next_sibling() : nb.first_child();
    for (auto c : mv) {
        if (first) nb.insert_move_before(c, first);
        else nb.append_move(c);
    }
    // a section break on the removed mark moves to the surviving one if it has none
    if (auto sp = p.child("w:pPr").child("w:sectPr")) {
        auto ppr = schema::ensure_child(nb, "w:pPr");
        if (!ppr.child("w:sectPr")) schema::place_child(ppr, ppr.append_move(sp));
    }
    if (p.attribute("w14:paraId") && !nb.attribute("w14:paraId")) {
        nb.prepend_attribute("w14:paraId") = p.attribute("w14:paraId").value();
    }
    p.parent().remove_child(p);
}

// ── property changes ─────────────────────────────────────────────────────

void resolve_prop_change(Ctx& c, pugi::xml_node ch) {
    c.hit(ch);
    auto owner = ch.parent();
    if (c.accept) {
        owner.remove_child(ch);
    } else {
        // restore the recorded properties: replace owner's children (except
        // revision markers that aren't ours) with those of the stored element
        auto stored = ch.first_child();
        while (stored && stored.type() != pugi::node_element) stored = stored.next_sibling();
        std::string_view oq = owner.name();
        std::vector<pugi::xml_node> drop;
        for (auto k : owner.children()) {
            std::string_view q = k.name();
            if (k == ch) continue;
            // keep the parts of the owner the change element doesn't cover
            if (oq == "w:pPr" && (q == "w:rPr" || q == "w:sectPr")) continue;
            if (oq == "w:rPr" && (q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo")) continue;
            if (oq == "w:trPr" && (q == "w:ins" || q == "w:del")) continue;
            if (oq == "w:tcPr" && (q == "w:cellIns" || q == "w:cellDel" || q == "w:cellMerge")) continue;
            if (oq == "w:sectPr" && (q == "w:headerReference" || q == "w:footerReference")) continue;
            drop.push_back(k);
        }
        for (auto k : drop) owner.remove_child(k);
        if (stored) {
            std::vector<pugi::xml_node> add;
            for (auto k : stored.children()) add.push_back(k);
            for (auto k : add) {
                // revision markers recorded in the old properties aren't properties
                std::string_view kq = k.name();
                if (kq == "w:cellIns" || kq == "w:cellDel" || kq == "w:cellMerge" || kq == "w:ins" || kq == "w:del")
                    continue;
                auto n = owner.append_copy(k);
                if (std::string_view(n.name()) == "w:numPr") n.remove_child("w:ins");
                schema::place_child(owner, n);
            }
            // stored attributes too (tblGrid has none; sectPr has rsid attrs)
            for (auto a : stored.attributes()) xml::set_attr(owner, a.name(), a.value());
        }
        owner.remove_child(ch);
        if (oq == "w:tblGrid") {
            // tblGridChange's child is a tblGrid: restore its gridCols
        }
    }
    // empty owners (rPr/pPr with nothing left) are removed by caller tidy
}

void resolve_numbering_change(Ctx& c, pugi::xml_node ch) {
    // w:numberingChange sits on a LISTNUM field's fldChar and records the
    // number the field showed before. Either way the marker goes; the field
    // recomputes its value.
    c.hit(ch);
    ch.parent().remove_child(ch);
}

// numPr/w:ins: numbering properties that were added to the paragraph.
void resolve_numpr_ins(Ctx& c, pugi::xml_node ins) {
    c.hit(ins);
    auto numpr = ins.parent();
    if (c.accept) {
        numpr.remove_child(ins);
    } else {
        auto ppr = numpr.parent();
        ppr.remove_child(numpr);
    }
}

// ── tables ───────────────────────────────────────────────────────────────

void resolve_row(Ctx& c, pugi::xml_node tr, pugi::xml_node rev, bool row_survives) {
    c.hit(rev);
    if (row_survives) {
        auto trpr = rev.parent();
        trpr.remove_child(rev);
        live_text(tr);
        if (!trpr.first_child()) tr.remove_child(trpr);
    } else {
        auto tbl = tr.parent();
        tbl.remove_child(tr);
        // a table without rows is removed
        if (!tbl.child("w:tr")) tbl.parent().remove_child(tbl);
    }
}

void resolve_cell(Ctx& c, pugi::xml_node tc, pugi::xml_node rev, bool cell_survives) {
    c.hit(rev);
    auto tcpr = rev.parent();
    if (cell_survives) {
        tcpr.remove_child(rev);
        live_text(tc);
        return;
    }
    // removing a cell: the cell before it absorbs its grid span (Word merges
    // the space into the previous cell)
    int span = std::max(1, xml::attr(tc.child("w:tcPr").child("w:gridSpan"), "w:val", "1") == "" ? 1 :
                           util::to_int(xml::attr(tc.child("w:tcPr").child("w:gridSpan"), "w:val", "1")));
    auto prev = tc.previous_sibling("w:tc");
    auto tr = tc.parent();
    tr.remove_child(tc);
    if (prev) {
        auto ptcpr = schema::ensure_child(prev, "w:tcPr");
        int ps = util::to_int(xml::attr(ptcpr.child("w:gridSpan"), "w:val", "1"));
        xml::set_attr(schema::ensure_child(ptcpr, "w:gridSpan"), "w:val", std::to_string(ps + span));
    }
}

void resolve_cell_merge(Ctx& c, pugi::xml_node cm) {
    c.hit(cm);
    auto tcpr = cm.parent();
    if (c.accept) {
        // vMerge="cont" -> vMerge continue; "rest" -> restart
        std::string vm = xml::attr(cm, "w:vMerge");
        if (!vm.empty()) {
            auto v = schema::ensure_child(tcpr, "w:vMerge");
            if (vm == "rest") xml::set_attr(v, "w:val", "restart");
            else xml::remove_attr(v, "w:val");
        }
    } else {
        std::string vmo = xml::attr(cm, "w:vMergeOrig");
        tcpr.remove_child("w:vMerge");
        if (vmo == "rest") xml::set_attr(schema::ensure_child(tcpr, "w:vMerge"), "w:val", "restart");
        else if (vmo == "cont") schema::ensure_child(tcpr, "w:vMerge");
    }
    tcpr.remove_child(cm);
}

// ── custom XML / content control ranges ──────────────────────────────────

void resolve_customxml_ranges(Ctx& c, pugi::xml_node root) {
    // customXmlInsRangeStart/End, customXmlDelRangeStart/End etc. mark the
    // insertion/deletion of the *markup* (sdt/customXml element) itself.
    struct R { const char* start; const char* end; bool ins; };
    const R ranges[] = {{"w:customXmlInsRangeStart", "w:customXmlInsRangeEnd", true},
                        {"w:customXmlDelRangeStart", "w:customXmlDelRangeEnd", false},
                        {"w:customXmlMoveFromRangeStart", "w:customXmlMoveFromRangeEnd", false},
                        {"w:customXmlMoveToRangeStart", "w:customXmlMoveToRangeEnd", true}};
    for (auto& r : ranges) {
        for (auto s : xml::descendants(root, r.start)) {
            if (!c.want(s)) continue;
            c.hit(s);
            bool markup_survives = r.ins == c.accept;
            std::string id = xml::attr(s, "w:id");
            // the marked element is the sdt/customXml right after the start
            // (or enclosing the end)
            pugi::xml_node target;
            for (auto n = s.next_sibling(); n; n = n.next_sibling()) {
                if (name_is(n, "w:sdt") || name_is(n, "w:customXml")) { target = n; break; }
                if (n.type() == pugi::node_element) break;
            }
            if (!target) {
                auto anc = s.parent();
                while (anc && !name_is(anc, "w:sdt") && !name_is(anc, "w:customXml")) anc = anc.parent();
                target = anc;
            }
            s.parent().remove_child(s);
            for (auto e : xml::descendants(root, r.end))
                if (xml::attr(e, "w:id") == id) e.parent().remove_child(e);
            if (!markup_survives && target) {
                // unwrap: keep content, drop the control
                if (name_is(target, "w:sdt")) {
                    auto content = target.child("w:sdtContent");
                    if (content) {
                        for (auto k = content.first_child(); k;) {
                            auto nx = k.next_sibling();
                            target.parent().insert_move_before(k, target);
                            k = nx;
                        }
                    }
                    target.parent().remove_child(target);
                } else {
                    std::vector<pugi::xml_node> keep;
                    for (auto k : target.children()) if (!name_is(k, "w:customXmlPr")) keep.push_back(k);
                    for (auto k : keep) target.parent().insert_move_before(k, target);
                    target.parent().remove_child(target);
                }
            }
        }
        for (auto e : xml::descendants(root, r.end)) {
            // dangling ends whose start was filtered stay
            (void)e;
        }
    }
}

// ── math ─────────────────────────────────────────────────────────────────

void resolve_math(Ctx& c, pugi::xml_node root) {
    // m:r/w:rPr/w:ins|w:del ; m:ctrlPr/w:ins|w:del (math control char revisions)
    for (auto q : {"w:ins", "w:del"}) {
        for (auto rev : xml::descendants(root, q)) {
            auto parent = rev.parent();
            if (!parent) continue;
            auto grand = parent.parent();
            bool in_math_rpr = name_is(parent, "w:rPr") && grand && (name_is(grand, "m:r") || name_is(grand, "m:ctrlPr"));
            bool in_ctrl = name_is(parent, "m:ctrlPr");
            bool math_run_wrapper = name_is(parent, "m:r");  // <m:r><w:del>...</w:del></m:r>
            if (!(in_math_rpr || in_ctrl || math_run_wrapper)) continue;
            if (!c.want(rev)) continue;
            bool is_ins = std::string_view(q) == "w:ins";
            bool survives = is_ins == c.accept;
            c.hit(rev);
            if (math_run_wrapper) {
                // the revision wraps the run's content
                if (survives) unwrap(rev);
                else parent.remove_child(rev);  // leaves <m:r/> as Word's accepted output does
                continue;
            }
            if (in_ctrl) {
                if (survives) unwrap(rev);
                else parent.remove_child(rev);
                continue;
            }
            // marker on the math run's properties
            parent.remove_child(rev);
            if (!survives) grand.parent().remove_child(grand);
        }
    }
}

// An equation whose every run went away is gone too; then a paragraph that
// held only that equation and is now empty behaves like any emptied one.
void drop_empty_math(pugi::xml_node root) {
    for (auto q : {"m:oMathPara", "m:oMath"})
        for (auto m : xml::descendants(root, q)) {
            if (!m.parent()) continue;
            bool text = false;
            for (auto t : xml::descendants(m, "m:t")) if (*t.text().get()) text = true;
            if (!text) m.parent().remove_child(m);
        }
}

// ── driver ───────────────────────────────────────────────────────────────

void run_pass(Ctx& c, pugi::xml_node root) {
    resolve_customxml_ranges(c, root);
    resolve_math(c, root);

    // 1. table rows and cells (before their content is touched). Removing a
    // row frees its nested tables, so each pass re-collects rather than
    // holding a list of nodes that may be gone.
    auto row_rev = [&](pugi::xml_node tr) -> bool {
        auto trpr = tr.child("w:trPr");
        if (!trpr) return false;
        // a row can be both inserted and deleted (by another author): the
        // deletion decides on accept, the insertion on reject
        auto ins = trpr.child("w:ins"), del = trpr.child("w:del");
        if (ins && del) {
            auto first = c.accept ? del : ins, second = c.accept ? ins : del;
            if (c.want(first)) {
                if (c.want(second)) c.hit(second);
                resolve_row(c, tr, first, false);
                return true;
            }
            if (c.want(second)) { c.hit(second); trpr.remove_child(second); }
            return false;
        }
        if (ins && c.want(ins)) { bool gone = !c.accept; resolve_row(c, tr, ins, c.accept); return gone; }
        if (del && c.want(del)) { bool gone = c.accept; resolve_row(c, tr, del, !c.accept); return gone; }
        return false;
    };
    // Deepest first: removing a nested row can't free one still to visit,
    // and removing an outer row frees only rows already handled.
    {
        auto trs = xml::descendants(root, "w:tr");
        for (auto it = trs.rbegin(); it != trs.rend(); ++it) row_rev(*it);
    }
    {
        // a cell is removed alone (never its row), so deepest-first is safe too
        auto tcs = xml::descendants(root, "w:tc");
        for (auto it = tcs.rbegin(); it != tcs.rend(); ++it) {
            auto tc = *it;
            auto tcpr = tc.child("w:tcPr");
            if (!tcpr) continue;
            if (auto ins = tcpr.child("w:cellIns"); ins && c.want(ins)) { resolve_cell(c, tc, ins, c.accept); continue; }
            if (auto del = tcpr.child("w:cellDel"); del && c.want(del)) { resolve_cell(c, tc, del, !c.accept); continue; }
            if (auto cm = tcpr.child("w:cellMerge"); cm && c.want(cm)) resolve_cell_merge(c, cm);
        }
    }

    // 2. run-level containers; innermost first so nested del-in-ins resolves right
    std::vector<pugi::xml_node> conts;
    std::function<void(pugi::xml_node)> collect = [&](pugi::xml_node n) {
        for (auto k : n.children()) {
            if (k.type() != pugi::node_element) continue;
            collect(k);
            std::string_view q = k.name();
            if ((q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo") &&
                !name_is(k.parent(), "w:rPr") && !name_is(k.parent(), "w:trPr") && !name_is(k.parent(), "m:ctrlPr") &&
                !name_is(k.parent(), "m:r") && !name_is(k.parent(), "w:numPr"))
                conts.push_back(k);
        }
    };
    collect(root);
    for (auto k : conts) {
        if (!k.parent()) continue;  // removed with an ancestor
        if (!c.want(k)) continue;
        std::string_view q = k.name();
        bool is_add = q == "w:ins" || q == "w:moveTo";
        resolve_container(c, k, is_add == c.accept);
    }
    drop_empty_math(root);
    // hyperlinks / simple fields / smart tags left with no runs were wholly
    // inside removed content; Word drops them too. Before the marks, so an
    // emptied paragraph is seen as empty.
    for (auto q : {"w:hyperlink", "w:fldSimple", "w:smartTag"})
        for (auto e : xml::descendants(root, q))
            if (e.parent() && !xml::first_descendant(e, "w:r") && !xml::first_descendant(e, "m:oMath"))
                e.parent().remove_child(e);

    // 3. paragraph marks (collect first; removing marks reshapes the tree)
    std::vector<std::pair<pugi::xml_node, pugi::xml_node>> marks;  // (p, rev)
    for (auto p : xml::descendants(root, "w:p")) {
        for (auto q : {"w:ins", "w:del", "w:moveFrom", "w:moveTo"}) {
            auto rev = mark_rev(p, q);
            if (rev && c.want(rev)) marks.emplace_back(p, rev);
        }
    }
    // process from the end so joins don't invalidate later paragraphs
    for (auto it = marks.rbegin(); it != marks.rend(); ++it) {
        auto [p, rev] = *it;
        if (!p.parent()) continue;
        std::string q = rev.name();
        bool is_add = q == "w:ins" || q == "w:moveTo";
        c.hit(rev);
        drop_mark_rev(p, q.c_str());
        if (is_add != c.accept) remove_mark(p);
    }

    // 4. property changes (after marks: they may have moved)
    for (auto& q : prop_changes()) {
        for (auto ch : xml::descendants(root, q)) {
            if (!ch.parent() || !c.want(ch)) continue;
            resolve_prop_change(c, ch);
        }
    }
    for (auto ch : xml::descendants(root, "w:numberingChange")) {
        if (!ch.parent() || !c.want(ch)) continue;
        resolve_numbering_change(c, ch);
    }
    for (auto ins : xml::descendants(root, "w:ins")) {
        if (!ins.parent() || !name_is(ins.parent(), "w:numPr") || !c.want(ins)) continue;
        resolve_numpr_ins(c, ins);
    }
    // empty rPr / pPr / trPr / tcPr left behind. Not inside a *PrChange: an
    // empty one there records "no formatting before" and is required.
    auto in_change = [](pugi::xml_node e) {
        std::string_view pq = e.parent().name();
        return pq.size() > 6 && pq.substr(pq.size() - 6) == "Change";
    };
    for (auto q : {"w:rPr", "w:trPr", "w:tblPrEx"}) {
        for (auto e : xml::descendants(root, q))
            if (!e.first_child() && !e.first_attribute() && !in_change(e)) e.parent().remove_child(e);
    }
    for (auto e : xml::descendants(root, "w:pPr"))
        if (!e.first_child() && !e.first_attribute() && !in_change(e)) e.parent().remove_child(e);

    if (!any_left(root, {"w:moveFrom", "w:moveTo"})) drop_move_ranges(root);
    for (auto p : xml::descendants(root, "w:p")) tidy(p);
    ensure_block_paragraphs(root);
}

}  // namespace

int accept_all_in(Document& d, pugi::xml_node root, const RevFilter& f) {
    Ctx c{d, f, true, 0, {}};
    run_pass(c, root);
    return c.count;
}

int reject_all_in(Document& d, pugi::xml_node root, const RevFilter& f) {
    Ctx c{d, f, false, 0, {}};
    run_pass(c, root);
    return c.count;
}

namespace {
std::vector<std::string> revision_parts(Document& d) {
    std::vector<std::string> v = d.story_parts();
    for (auto rt : {opc::rt::styles, opc::rt::numbering, opc::rt::settings}) {
        auto n = d.related(rt);
        if (!n.empty()) v.push_back(n);
    }
    return v;
}
}  // namespace

int accept_revisions(Document& d, const RevFilter& f) {
    int n = 0;
    for (auto& part : revision_parts(d))
        if (d.pkg().has(part)) n += accept_all_in(d, d.pkg().xml(part).document_element(), f);
    return n;
}

int reject_revisions(Document& d, const RevFilter& f) {
    int n = 0;
    for (auto& part : revision_parts(d))
        if (d.pkg().has(part)) n += reject_all_in(d, d.pkg().xml(part).document_element(), f);
    return n;
}

std::vector<RevisionInfo> list_revisions(Document& d) {
    std::vector<RevisionInfo> out;
    static const std::set<std::string> kinds = {
        "w:ins", "w:del", "w:moveFrom", "w:moveTo", "w:rPrChange", "w:pPrChange", "w:sectPrChange", "w:tblPrChange",
        "w:trPrChange", "w:tcPrChange", "w:tblGridChange", "w:tblPrExChange", "w:numberingChange", "w:cellIns",
        "w:cellDel", "w:cellMerge", "w:customXmlInsRangeStart", "w:customXmlDelRangeStart",
        "w:customXmlMoveFromRangeStart", "w:customXmlMoveToRangeStart"};
    for (auto& part : revision_parts(d)) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
            for (auto k : n.children()) {
                if (k.type() != pugi::node_element) continue;
                std::string q = k.name();
                if (kinds.count(q)) {
                    RevisionInfo r;
                    r.id = xml::attr(k, "w:id");
                    r.author = xml::attr(k, "w:author");
                    r.date = xml::attr(k, "w:date");
                    r.part = part;
                    r.node = k;
                    auto parent = k.parent();
                    std::string pq = parent.name();
                    if (q == "w:ins" || q == "w:del" || q == "w:moveFrom" || q == "w:moveTo") {
                        std::string base = q == "w:ins" ? "insertion" : q == "w:del" ? "deletion" : q == "w:moveFrom" ? "move_from" : "move_to";
                        if (pq == "w:rPr" && name_is(parent.parent(), "w:pPr")) r.type = "paragraph_mark_" + base;
                        else if (pq == "w:trPr") r.type = "table_row_" + base;
                        else if (pq == "w:rPr" || pq == "m:ctrlPr" || pq == "m:r") r.type = "math_" + base;
                        else {
                            r.type = base;
                            std::string t;
                            for (auto tx : k.select_nodes(".//w:t|.//w:delText|.//w:tab|.//w:br")) {
                                std::string_view tn = tx.node().name();
                                if (tn == "w:tab") t += "\t";
                                else if (tn == "w:br") t += "\n";
                                else t += tx.node().text().get();
                            }
                            r.text = t;
                        }
                    } else if (q == "w:rPrChange") r.type = "run_format";
                    else if (q == "w:pPrChange") r.type = "paragraph_format";
                    else if (q == "w:sectPrChange") r.type = "section_format";
                    else if (q == "w:tblPrChange" || q == "w:tblPrExChange") r.type = "table_format";
                    else if (q == "w:trPrChange") r.type = "table_row_format";
                    else if (q == "w:tcPrChange") r.type = "table_cell_format";
                    else if (q == "w:tblGridChange") r.type = "table_grid";
                    else if (q == "w:numberingChange") r.type = "numbering";
                    else if (q == "w:cellIns") r.type = "table_cell_insertion";
                    else if (q == "w:cellDel") r.type = "table_cell_deletion";
                    else if (q == "w:cellMerge") r.type = "table_cell_merge";
                    else r.type = "markup_" + q.substr(2);
                    for (auto a = k; a; a = a.parent())
                        if (name_is(a, "w:p")) { r.para = xml::attr(a, "w14:paraId"); break; }
                    out.push_back(r);
                }
                walk(k);
            }
        };
        walk(root);
    }
    return out;
}

}  // namespace docx::wml

#include "wml/comments.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/edit.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

pugi::xml_node comment_by_id(Document& d, const std::string& id) {
    auto cs = d.comments(false);
    if (!cs) return {};
    for (auto c : cs.children("w:comment"))
        if (xml::attr(c, "w:id") == id) return c;
    return {};
}

std::string last_para_id(pugi::xml_node c) {
    pugi::xml_node last;
    for (auto p : c.children("w:p")) last = p;
    return xml::attr(last, "w14:paraId");
}

pugi::xml_node ex_for(Document& d, const std::string& para_id) {
    auto ex = d.comments_ex(false);
    if (!ex) return {};
    for (auto e : ex.children("w15:commentEx"))
        if (util::iequals(xml::attr(e, "w15:paraId"), para_id)) return e;
    return {};
}

std::string initials_of(const std::string& name) {
    std::string s;
    bool start = true;
    for (char c : name) {
        if (c == ' ') { start = true; continue; }
        if (start && std::isalpha((unsigned char)c)) s += char(std::toupper((unsigned char)c));
        start = false;
    }
    return s.empty() ? "A" : s;
}

// Build the w:comment element Word writes. Text lines become paragraphs.
pugi::xml_node make_comment(Document& d, const std::string& id, const std::string& text) {
    auto cs = d.comments();
    ensure_style(d, "CommentText");
    ensure_style(d, "CommentReference");
    auto c = cs.append_child("w:comment");
    xml::set_attr(c, "w:id", id);
    xml::set_attr(c, "w:author", d.author.name);
    xml::set_attr(c, "w:date", d.date());
    xml::set_attr(c, "w:initials", d.author.initials.empty() ? initials_of(d.author.name) : d.author.initials);
    auto lines = util::split(text, '\n');
    for (std::size_t i = 0; i < lines.size(); i++) {
        auto p = c.append_child("w:p");
        p.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        p.append_attribute("w14:textId") = "77777777";
        xml::set_attr(schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:pStyle"), "w:val", "CommentText");
        if (i == 0) {
            auto r = p.append_child("w:r");
            xml::set_attr(schema::ensure_child(schema::ensure_child(r, "w:rPr"), "w:rStyle"), "w:val", "CommentReference");
            r.append_child("w:annotationRef");
        }
        if (!lines[i].empty()) {
            auto r = p.append_child("w:r");
            append_text(r, util::to_u32(lines[i]));
        }
    }
    xml::declare(cs, "w14", true);
    return c;
}

// Word's companion records for a new comment.
void add_companions(Document& d, pugi::xml_node c, const std::string& parent_para) {
    std::string pid = last_para_id(c);
    auto ex = d.comments_ex();
    auto e = ex.append_child("w15:commentEx");
    xml::set_attr(e, "w15:paraId", pid);
    if (!parent_para.empty()) xml::set_attr(e, "w15:paraIdParent", parent_para);
    xml::set_attr(e, "w15:done", "0");
    std::string durable = d.new_hex_id();
    auto ids = d.comments_ids();
    auto ci = ids.append_child("w16cid:commentId");
    xml::set_attr(ci, "w16cid:paraId", pid);
    xml::set_attr(ci, "w16cid:durableId", durable);
    auto cx = d.comments_extensible();
    auto ce = cx.append_child("w16cex:commentExtensible");
    xml::set_attr(ce, "w16cex:durableId", durable);
    xml::set_attr(ce, "w16cex:dateUtc", d.date());
    d.ensure_person(d.author.name);
}

// Insert commentRangeStart before visible pos s of p1 and commentRangeEnd +
// reference run after visible pos e of p2.
void anchor(Document& d, pugi::xml_node p1, int s, pugi::xml_node p2, int e, const std::string& id) {
    auto place = [&](pugi::xml_node p, int pos, const char* q) -> pugi::xml_node {
        split_at(p, pos);
        ParaText pt = analyze(p);
        int n = (int)pt.vis.size();
        auto top = [&](pugi::xml_node node) {
            while (std::string_view(node.parent().name()) != "w:p") node = node.parent();
            return node;
        };
        if (pos < n) return p.insert_child_before(q, top(pt.atoms[pt.vis[pos]].run));
        if (n > 0) return p.insert_child_after(q, top(pt.atoms[pt.vis[n - 1]].run));
        return p.append_child(q);
    };
    auto st = place(p1, s, "w:commentRangeStart");
    xml::set_attr(st, "w:id", id);
    // end: after the character at e-1
    pugi::xml_node en;
    {
        split_at(p2, e);
        ParaText pt = analyze(p2);
        int n = (int)pt.vis.size();
        if (e == 0 || n == 0) {
            if (p1 == p2) en = p2.insert_child_after("w:commentRangeEnd", st);
            else {
                auto first = p2.child("w:pPr") ? p2.child("w:pPr").next_sibling() : p2.first_child();
                en = first ? p2.insert_child_before("w:commentRangeEnd", first) : p2.append_child("w:commentRangeEnd");
            }
        } else {
            auto node = pt.atoms[pt.vis[std::min(e, n) - 1]].run;
            while (std::string_view(node.parent().name()) != "w:p") node = node.parent();
            en = p2.insert_child_after("w:commentRangeEnd", node);
        }
    }
    xml::set_attr(en, "w:id", id);
    auto r = p2.insert_child_after("w:r", en);
    xml::set_attr(schema::ensure_child(schema::ensure_child(r, "w:rPr"), "w:rStyle"), "w:val", "CommentReference");
    xml::set_attr(r.append_child("w:commentReference"), "w:id", id);
    // a comment made while tracking marks its reference run as inserted (Word)
    if (d.track_changes()) {
        auto ins = p2.insert_child_before("w:ins", r);
        stamp(d, ins);
        ins.append_move(r);
    }
}

std::string comment_text(pugi::xml_node c) {
    std::string s;
    bool first = true;
    for (auto p : c.children("w:p")) {
        if (!first) s += "\n";
        first = false;
        s += analyze(p).utf8();
    }
    return s;
}

}  // namespace

std::vector<CommentInfo> list_comments(Document& d) {
    std::vector<CommentInfo> out;
    auto cs = d.comments(false);
    if (!cs) return out;
    // map paraId -> comment id, for threading
    std::map<std::string, std::string> by_para;
    for (auto c : cs.children("w:comment")) by_para[util::lower(last_para_id(c))] = xml::attr(c, "w:id");
    // anchors
    std::map<std::string, std::pair<std::string, std::string>> anchors;  // id -> (para, text)
    std::map<std::string, std::string> anchor_part;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto p : paragraphs(root)) {
            for (auto s : xml::children(p, "w:commentRangeStart")) {
                std::string id = xml::attr(s, "w:id");
                anchors[id].first = xml::attr(p, "w14:paraId");
                anchor_part[id] = part;
            }
        }
        // anchored text: walk the story in order collecting text between start/end
        std::map<std::string, bool> open;
        std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
            for (auto k : n.children()) {
                if (k.type() != pugi::node_element) continue;
                std::string_view q = k.name();
                if (q == "w:commentRangeStart") { open[xml::attr(k, "w:id")] = true; continue; }
                if (q == "w:commentRangeEnd") { open.erase(xml::attr(k, "w:id")); continue; }
                if (q == "w:t" && !open.empty()) {
                    for (auto& [id, _] : open) anchors[id].second += k.text().get();
                    continue;
                }
                if (q == "w:p") {
                    walk(k);
                    // a paragraph mark inside an open range reads as a newline
                    for (auto& [id, _] : open) anchors[id].second += "\n";
                    continue;
                }
                if (q == "w:delText" || q == "w:instrText" || q == "mc:Fallback") continue;
                walk(k);
            }
        };
        walk(root);
    }
    for (auto c : cs.children("w:comment")) {
        CommentInfo i;
        i.id = xml::attr(c, "w:id");
        i.author = xml::attr(c, "w:author");
        i.initials = xml::attr(c, "w:initials");
        i.date = xml::attr(c, "w:date");
        i.text = comment_text(c);
        i.para_id = last_para_id(c);
        if (auto e = ex_for(d, i.para_id)) {
            i.done = xml::attr(e, "w15:done") == "1";
            auto par = util::lower(xml::attr(e, "w15:paraIdParent"));
            if (!par.empty() && by_para.count(par)) i.parent_id = by_para[par];
        }
        if (auto it = anchors.find(i.id); it != anchors.end()) {
            i.anchor_para = it->second.first;
            i.anchor_text = it->second.second;
            while (!i.anchor_text.empty() && i.anchor_text.back() == '\n') i.anchor_text.pop_back();
        }
        i.part = anchor_part[i.id];
        out.push_back(i);
    }
    return out;
}

std::string add_comment_range(Document& d, pugi::xml_node p1, int s, pugi::xml_node p2, int e, const std::string& text) {
    if (text.empty()) throw std::runtime_error("comment text is empty");
    std::string id = std::to_string(d.next_annotation_id());
    auto c = make_comment(d, id, text);
    add_companions(d, c, "");
    anchor(d, p1, s, p2, e, id);
    return id;
}

std::string add_comment(Document& d, pugi::xml_node p, int s, int e, const std::string& text) {
    int n = (int)analyze(p).vis.size();
    if (s < 0 || e > n || s > e) throw std::runtime_error("comment range outside paragraph (length " + std::to_string(n) + ")");
    return add_comment_range(d, p, s, p, e, text);
}

std::string reply_comment(Document& d, const std::string& parent_id, const std::string& text) {
    auto parent = comment_by_id(d, parent_id);
    if (!parent) throw std::runtime_error("comment " + parent_id + " not found");
    // replies thread to the root comment
    std::string root_para = last_para_id(parent);
    if (auto e = ex_for(d, root_para); e && !xml::attr(e, "w15:paraIdParent").empty())
        root_para = xml::attr(e, "w15:paraIdParent");
    std::string id = std::to_string(d.next_annotation_id());
    auto c = make_comment(d, id, text);
    add_companions(d, c, root_para);
    // Word anchors a reply on the same range as its parent.
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto s : xml::descendants(root, "w:commentRangeStart")) {
            if (xml::attr(s, "w:id") != parent_id) continue;
            auto ns = s.parent().insert_child_after("w:commentRangeStart", s);
            xml::set_attr(ns, "w:id", id);
        }
        for (auto e : xml::descendants(root, "w:commentRangeEnd")) {
            if (xml::attr(e, "w:id") != parent_id) continue;
            auto ne = e.parent().insert_child_after("w:commentRangeEnd", e);
            xml::set_attr(ne, "w:id", id);
        }
        for (auto ref : xml::descendants(root, "w:commentReference")) {
            if (xml::attr(ref, "w:id") != parent_id) continue;
            auto run = ref.parent();
            auto nr = run.parent().insert_copy_after(run, run);
            xml::set_attr(nr.child("w:commentReference"), "w:id", id);
        }
    }
    return id;
}

void resolve_comment(Document& d, const std::string& id, bool done) {
    auto c = comment_by_id(d, id);
    if (!c) throw std::runtime_error("comment " + id + " not found");
    std::string pid = last_para_id(c);
    auto e = ex_for(d, pid);
    if (!e) {
        auto ex = d.comments_ex();
        e = ex.append_child("w15:commentEx");
        xml::set_attr(e, "w15:paraId", pid);
    }
    xml::set_attr(e, "w15:done", done ? "1" : "0");
}

void edit_comment(Document& d, const std::string& id, const std::string& text) {
    auto c = comment_by_id(d, id);
    if (!c) throw std::runtime_error("comment " + id + " not found");
    std::string old_pid = last_para_id(c);
    // keep the first paragraph (with annotationRef) and its paraId chain
    auto lines = util::split(text, '\n');
    std::vector<pugi::xml_node> ps;
    for (auto p : c.children("w:p")) ps.push_back(p);
    for (std::size_t i = 1; i < ps.size(); i++) c.remove_child(ps[i]);
    auto p0 = ps.empty() ? c.append_child("w:p") : ps[0];
    for (auto k = p0.first_child(); k;) {
        auto nx = k.next_sibling();
        if (std::string_view(k.name()) == "w:r" && !k.child("w:annotationRef")) p0.remove_child(k);
        k = nx;
    }
    if (!lines.empty() && !lines[0].empty()) append_text(p0.append_child("w:r"), util::to_u32(lines[0]));
    pugi::xml_node last = p0;
    for (std::size_t i = 1; i < lines.size(); i++) {
        auto p = c.append_child("w:p");
        p.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        p.append_attribute("w14:textId") = "77777777";
        xml::set_attr(schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:pStyle"), "w:val", "CommentText");
        if (!lines[i].empty()) append_text(p.append_child("w:r"), util::to_u32(lines[i]));
        last = p;
    }
    // the comment's identity paraId is its last paragraph: keep it stable
    std::string new_pid = xml::attr(last, "w14:paraId");
    if (!util::iequals(new_pid, old_pid)) {
        xml::set_attr(last, "w14:paraId", old_pid);
        if (last != p0) xml::set_attr(p0, "w14:paraId", new_pid);
    }
}

namespace {
void drop_markers(Document& d, const std::set<std::string>& ids) {
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto q : {"w:commentRangeStart", "w:commentRangeEnd"})
            for (auto n : xml::descendants(root, q))
                if (ids.count(xml::attr(n, "w:id"))) n.parent().remove_child(n);
        for (auto ref : xml::descendants(root, "w:commentReference")) {
            if (!ids.count(xml::attr(ref, "w:id"))) continue;
            auto run = ref.parent();
            run.remove_child(ref);
            bool content = false;
            for (auto k : run.children())
                if (k.type() == pugi::node_element && std::string_view(k.name()) != "w:rPr") content = true;
            auto parent = run.parent();
            if (!content) parent.remove_child(run);
            if (is_revision_container(parent) && !parent.first_child()) parent.parent().remove_child(parent);
        }
    }
}
}  // namespace

void delete_comment(Document& d, const std::string& id) {
    auto c = comment_by_id(d, id);
    if (!c) throw std::runtime_error("comment " + id + " not found");
    // collect the comment and its replies
    std::set<std::string> ids{id};
    std::set<std::string> paras{util::lower(last_para_id(c))};
    auto all = list_comments(d);
    for (auto& ci : all) if (ci.parent_id == id) { ids.insert(ci.id); paras.insert(util::lower(ci.para_id)); }
    drop_markers(d, ids);
    auto cs = d.comments();
    for (auto& cid : ids) if (auto n = comment_by_id(d, cid)) cs.remove_child(n);
    std::set<std::string> durables;
    if (auto ex = d.comments_ex(false))
        for (auto e : xml::children(ex, "w15:commentEx"))
            if (paras.count(util::lower(xml::attr(e, "w15:paraId")))) ex.remove_child(e);
    if (auto ci = d.comments_ids(false))
        for (auto e : xml::children(ci, "w16cid:commentId"))
            if (paras.count(util::lower(xml::attr(e, "w16cid:paraId")))) {
                durables.insert(util::lower(xml::attr(e, "w16cid:durableId")));
                ci.remove_child(e);
            }
    if (auto cx = d.comments_extensible(false))
        for (auto e : xml::children(cx, "w16cex:commentExtensible"))
            if (durables.count(util::lower(xml::attr(e, "w16cex:durableId")))) cx.remove_child(e);
}

int delete_all_comments(Document& d, const std::string& author) {
    int n = 0;
    for (auto& c : list_comments(d)) {
        if (!c.parent_id.empty()) continue;  // removed with the root
        if (!author.empty() && c.author != author) continue;
        if (!comment_by_id(d, c.id)) continue;
        delete_comment(d, c.id);
        n++;
    }
    return n;
}

}  // namespace docx::wml

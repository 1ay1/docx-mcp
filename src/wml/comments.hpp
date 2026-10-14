// Comments, the way Word 365 stores them: comments.xml plus
// commentsExtended (threads, resolved), commentsIds (durable ids),
// commentsExtensible (UTC dates) and people.xml (authors).
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "wml/document.hpp"

namespace docx::wml {

struct CommentInfo {
    std::string id;         // w:id
    std::string author, initials, date;
    std::string text;
    std::string para_id;    // w14:paraId of the comment's last paragraph
    std::string parent_id;  // w:id of the comment this replies to ("" if none)
    bool done = false;
    std::string anchor_text;   // commented text in the document
    std::string anchor_para;   // paraId of the paragraph where the range starts
    std::string part;          // story holding the anchor
};

std::vector<CommentInfo> list_comments(Document& d);

// Add a comment on visible range [s, e) of paragraph p (s == e: a point).
// Returns the new comment's w:id.
std::string add_comment(Document& d, pugi::xml_node p, int s, int e, const std::string& text);
// Range spanning paragraphs: [s in p1, e in p2).
std::string add_comment_range(Document& d, pugi::xml_node p1, int s, pugi::xml_node p2, int e,
                              const std::string& text);
// Reply to an existing comment (threaded, like Word's Reply button).
std::string reply_comment(Document& d, const std::string& parent_id, const std::string& text);
void resolve_comment(Document& d, const std::string& id, bool done = true);
// Delete a comment (and its replies, like Word).
void delete_comment(Document& d, const std::string& id);
void edit_comment(Document& d, const std::string& id, const std::string& text);
int delete_all_comments(Document& d, const std::string& author = "");

}  // namespace docx::wml

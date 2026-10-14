// Text model: paragraphs as sequences of visible characters ("atoms") mapped
// back to the XML that holds them, plus the run surgery every edit needs.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <pugixml.hpp>

namespace docx::wml {

class Document;

enum class Rev : std::uint8_t { None, Ins, Del, MoveFrom, MoveTo };

struct Atom {
    pugi::xml_node run;   // w:r (or m:oMath for an atomic math object)
    pugi::xml_node node;  // the child carrying the char (w:t, w:tab, ...)
    int off = 0;          // offset within a w:t/w:delText (code points)
    char32_t ch = 0;
    Rev rev = Rev::None;
    pugi::xml_node rev_node;  // innermost w:ins/w:del/w:moveFrom/w:moveTo
    pugi::xml_node ins_node;  // enclosing w:ins (when a deletion sits inside an insertion)
    bool field_code = false;  // inside a field instruction (hidden)
    bool object = false;      // drawing, note reference, math, ...
};

// Word's text conventions (Range.Text): line break \v, page/column break \f,
// tab \t, objects U+FFFC.
inline constexpr char32_t kObj = 0xFFFC;

struct ParaText {
    std::vector<Atom> atoms;       // every atom, in order
    std::vector<int> vis;          // indices of visible atoms (current view)
    std::u32string text;           // visible text (current view)
    std::string utf8() const;
};

// Visible = not deleted / moved away and not field instruction text.
ParaText analyze(pugi::xml_node p);
// Original view: what the paragraph read before tracked changes.
std::u32string original_text(pugi::xml_node p);

// Paragraphs of a story root in reading order (skips mc:Fallback copies).
std::vector<pugi::xml_node> paragraphs(pugi::xml_node root, bool include_textboxes = true);
// Block-level children in order (w:p and w:tbl, descending into sdt/customXml).
std::vector<pugi::xml_node> blocks(pugi::xml_node container);

// Stable paragraph id: w14:paraId, or a deterministic one derived from the
// paragraph's position when missing (written on next save).
std::string para_id(pugi::xml_node p);
void assign_missing_para_ids(Document& d, pugi::xml_node root, const std::string& part);

struct ParaRef {
    pugi::xml_node p;
    std::string part;
};
// Find a paragraph by w14:paraId (hex) or 1-based index in the main story
// ("12"), or "part:/word/footnotes.xml#3" style refs. Throws if not found.
ParaRef find_para(Document& d, const std::string& ref);

// ── run surgery ──
// Make sure visible position `pos` (0..len) starts a new run. Returns the
// atom index of the first atom at/after pos in the re-analyzed paragraph.
void split_at(pugi::xml_node p, int pos);
// Runs covering visible range [s, e) after splitting at both ends.
std::vector<pugi::xml_node> isolate(pugi::xml_node p, int s, int e);

// Append text to a run as w:t/w:tab/w:br/... honoring \t \v \f.
void append_text(pugi::xml_node run, const std::u32string& text, bool deleted = false);
// New run with a copy of `rpr` (may be empty) and the text.
pugi::xml_node make_run(pugi::xml_node parent, pugi::xml_node before, pugi::xml_node rpr,
                        const std::u32string& text, bool deleted = false);
// Copy of the run properties suitable for new text (no revision markers).
void copy_rpr(pugi::xml_node dst_run, pugi::xml_node src_rpr);
// Paragraph-mark run properties as a run rPr (minus ins/del/rPrChange).
void copy_mark_rpr(pugi::xml_node dst_run, pugi::xml_node p);

bool is_revision_container(pugi::xml_node n);
// Merge adjacent runs with identical properties (and adjacent same revision
// containers) — keeps the XML tidy after edits.
void tidy(pugi::xml_node p);

}  // namespace docx::wml

# Tool reference

Generated from `docx-mcp --list-tools` by `tools/gen_tool_docs.py`. 90 tools.

Every tool that edits takes `doc` (a handle or a file path), and most take `track` (record as a tracked change) and `author`. Paragraphs are addressed by the `id` that `read_document` returns.

## Contents

- [Documents](#documents): `open_document`, `create_document`, `save_document`, `close_document`, `list_open_documents`, `undo`
- [Reading](#reading): `read_document`, `get_text`, `get_outline`, `get_paragraph`, `find_text`, `get_statistics`
- [Editing text](#editing-text): `insert_text`, `replace_text`, `delete_text`, `find_replace`, `insert_paragraph`, `delete_paragraph`, `split_paragraph`, `merge_paragraphs`, `move_paragraphs`
- [Formatting](#formatting): `format_text`, `format_paragraph`, `apply_style`, `clear_formatting`
- [Styles](#styles): `list_styles`, `get_style`, `modify_style`, `delete_style`
- [Lists](#lists): `apply_list`, `set_list_level`, `remove_list`, `restart_numbering`
- [Track changes](#track-changes): `set_track_changes`, `list_revisions`, `accept_revisions`, `reject_revisions`, `compare_documents`
- [Comments](#comments): `add_comment`, `list_comments`, `reply_comment`, `resolve_comment`, `edit_comment`, `delete_comment`
- [Tables](#tables): `insert_table`, `read_table`, `set_cell`, `table_rows`, `table_columns`, `merge_cells`, `format_table`, `delete_table`
- [Insert](#insert): `insert_picture`, `insert_break`, `insert_hyperlink`, `list_hyperlinks`, `remove_hyperlinks`, `add_bookmark`, `list_bookmarks`, `delete_bookmark`, `insert_note`, `list_notes`, `delete_note`, `insert_field`, `list_fields`, `update_fields`, `insert_toc`, `insert_equation`, `insert_caption`, `insert_cross_reference`, `list_reference_targets`, `add_content_control`, `list_content_controls`, `set_content_control`
- [Page layout](#page-layout): `get_page_setup`, `set_page_setup`, `set_header_footer`, `remove_header_footer`, `read_header_footer`
- [File](#file): `get_properties`, `set_properties`, `get_settings`, `set_settings`, `protect_document`, `unprotect_document`
- [Expert](#expert): `validate_document`, `get_xml`, `set_paragraph_xml`
- [Finding tools](#finding-tools): `help`, `run_tool`

## Documents

### `open_document`

Open a .docx file for reading/editing. Returns a handle. Opening an already open path returns the same handle.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Name used for tracked changes and comments (default from server). |
| `create_if_missing` | boolean |  | Create a blank document when the file doesn't exist. |
| `path` | string | yes | Path to the .docx file. |

### `create_document`

Create a new blank document exactly like Word's File > New > Blank document (Calibri 11, Office theme, Letter). Saved to `path` on save_document.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author for revisions/comments and document properties. |
| `paper` | `letter` \\| `a4` \\| `legal` \\| `a5` \\| `a3` |  | Paper size (default letter). |
| `path` | string | yes | Where save_document will write it. |
| `title` | string |  | Document title property. |

### `save_document`

Write the document to disk (atomic replace). `path` saves a copy elsewhere (Save As) and retargets the handle.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `path` | string |  | Save As path (optional). |
| `validate` | boolean |  | Refuse to save if the result is not schema-valid (default true). |

### `close_document`

Close a document without saving.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `list_open_documents`

List the documents currently open in this server.

### `undo`

Undo the last change (repeatable, up to 30 steps).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `steps` | integer |  | How many (default 1). |

## Reading

### `read_document`

Read paragraphs with their ids, styles, list labels, table positions and revision/comment markers. Paginate with offset/limit. view=original shows the text before tracked changes.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `limit` | integer |  | Max paragraphs (default 200). |
| `offset` | integer |  | 1-based first paragraph (negative = from the end). |
| `runs` | boolean |  | Include formatted runs with offsets. |
| `view` | `current` \\| `original` |  | current = with changes applied (Word's 'Simple Markup'); original = before changes. |

### `get_text`

The whole document as plain text or Markdown (headings, lists, tables).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | `text` \\| `markdown` |  | Default markdown. |
| `view` | `current` \\| `original` |  |  |

### `get_outline`

Show the document outline: every heading with its level and paragraph id (the Navigation Pane). Use it to see the structure and jump to a section.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `get_paragraph`

One paragraph: text, runs with formatting and offsets, direct and effective (style-resolved) formatting, list label.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |

### `find_text`

Find text across the document (body, headers, footers, notes, comments). Returns paragraph ids and offsets for editing.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `limit` | integer |  | Max hits (default 100). |
| `match_case` | boolean |  | Default true. |
| `regex` | boolean |  | ECMAScript regex. |
| `text` | string | yes | Text or regex to find. |
| `whole_word` | boolean |  |  |

### `get_statistics`

Word's Word Count: words, characters (with/without spaces), paragraphs, plus tables/comments/revisions/sections.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

## Editing text

### `insert_text`

Type text into a paragraph at a position (default: end), or before/after given text. New text takes the formatting of the character before it, like typing in Word. \t = tab, \v = line break.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `author` | string |  | Author of this change. |
| `before` | string |  | Insert before this text. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `occurrence` | integer |  | Which occurrence of after/before (default 1). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset (negative counts from end). |
| `text` | string | yes | Text to insert. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `replace_text`

Replace text inside one paragraph (by match/occurrence or start/end). Tracked as deletion + insertion when tracking.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `end` | integer |  | End character offset. |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `match` | string |  | Text to replace. |
| `occurrence` | integer |  | Default 1. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `start` | integer |  | Start character offset. |
| `text` | string | yes | Replacement text (empty deletes). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `delete_text`

Delete text inside a paragraph (match/occurrence or start/end).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `end` | integer |  | End character offset. |
| `match` | string |  | Exact text to act on. |
| `occurrence` | integer |  | Which occurrence of match (default 1). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `start` | integer |  | Start character offset. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `find_replace`

Replace All across the document (body, headers, footers, notes). Keeps the formatting of each match. Regex replacements support $1..$9.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `find` | string | yes | Text or regex. |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `limit` | integer |  | Max replacements. |
| `match_case` | boolean |  | Default true. |
| `regex` | boolean |  |  |
| `replace` | string | yes | Replacement. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `whole_word` | boolean |  |  |

### `insert_paragraph`

Insert new paragraph(s) before or after a paragraph (or a table: pass table). Lines in `text` become separate paragraphs. Inherits the neighbour's paragraph formatting unless `style` is given.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `paragraph_format` | object |  | Paragraph formatting (Word names). Lengths in points or with a unit ("0.5in", "1cm"). null removes. More keys: line_rule, first_line_indent, hanging_indent, keep_together, page_break_before, widow_control, outline_level, shading, borders, tabs, bidi. |
| `style` | string |  | Paragraph style id or name, e.g. Heading1 / "Heading 1" / Quote. |
| `table` | integer |  | Anchor on this table (1-based) instead of a paragraph. |
| `text` | string |  | Text; \n separates paragraphs. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `where` | `after` \\| `before` |  | Default after. |

### `delete_paragraph`

Delete whole paragraphs (text and paragraph mark).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraphs` | array of string | yes | Paragraph ids/numbers. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `split_paragraph`

Press Enter at a position: splits the paragraph in two.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Split after this text. |
| `author` | string |  | Author of this change. |
| `before` | string |  | Split before this text. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `merge_paragraphs`

Delete the paragraph mark, joining the paragraph with the next one.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `move_paragraphs`

Cut paragraphs and paste them elsewhere (before/after a target). Untracked moves relocate the XML; tracked moves are recorded as a deletion plus an insertion.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraphs` | array of string | yes | Paragraph ids in order. |
| `target` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `where` | `after` \\| `before` |  | after or before. |

## Formatting

### `format_text`

Apply character formatting to text in a paragraph (match or start/end; default the whole paragraph).

| Argument | Type | Required | Description |
|---|---|---|---|
| `all_occurrences` | boolean |  | Format every occurrence of match in the paragraph. |
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `end` | integer |  | End character offset. |
| `format` | object | yes | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `match` | string |  | Exact text to act on. |
| `occurrence` | integer |  | Which occurrence of match (default 1). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `start` | integer |  | Start character offset. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `format_paragraph`

Apply paragraph formatting / style to one or more paragraphs.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object | yes | Paragraph formatting (Word names). Lengths in points or with a unit ("0.5in", "1cm"). null removes. More keys: line_rule, first_line_indent, hanging_indent, keep_together, page_break_before, widow_control, outline_level, shading, borders, tabs, bidi. |
| `paragraphs` | array of string | yes | Paragraph ids/numbers. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `apply_style`

Apply a paragraph or character style by id or name ("Heading 1", "Title", "Quote", "Strong"...).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `match` | string |  | For a character style: text to style. |
| `paragraphs` | array of string | yes | Paragraph ids or numbers. |
| `style` | string | yes | Style name or id. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `clear_formatting`

Remove direct character formatting (and optionally paragraph formatting), like Ctrl+Space / Ctrl+Q.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph_too` | boolean |  | Also reset paragraph formatting to the style. |
| `paragraphs` | array of string | yes | Paragraph ids or numbers. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

## Styles

### `list_styles`

Styles in the document; include_builtin adds Word's built-ins not yet used (added on first use).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `include_builtin` | boolean |  |  |
| `type` | `paragraph` \\| `character` \\| `table` \\| `numbering` |  | Filter. |

### `get_style`

Show a style's settings: font, size, color, spacing, based on, next style. Works for Word's built-in styles too.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `style` | string | yes | Style name or id. |

### `modify_style`

Create a new style or modify one (Modify Style dialog). Changes apply everywhere the style is used.

| Argument | Type | Required | Description |
|---|---|---|---|
| `based_on` | string |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `hidden` | boolean |  |  |
| `name` | string |  | Name. |
| `next` | string |  | Style for the next paragraph. |
| `paragraph` | object |  | Paragraph formatting (Word names). Lengths in points or with a unit ("0.5in", "1cm"). null removes. More keys: line_rule, first_line_indent, hanging_indent, keep_together, page_break_before, widow_control, outline_level, shading, borders, tabs, bidi. |
| `quick_style` | boolean |  | Show in the Styles gallery. |
| `run` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `style` | string |  | Style id or name; a new id is derived from name when creating. |
| `type` | `paragraph` \\| `character` \\| `table` \\| `numbering` |  | For new styles. |

### `delete_style`

Delete a style; text using it reverts to Normal / Default Paragraph Font.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `style` | string | yes | Style name or id. |

## Lists

### `apply_list`

Make paragraphs a bulleted or numbered list (Word's default list definitions). Continues an adjacent list of the same kind.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `bullet` | string |  | Bullet character. |
| `continue_previous` | boolean |  | Default true. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `bullet` \\| `number` \\| `lowerLetter` \\| `upperLetter` \\| `lowerRoman` \\| `upperRoman` | yes | Kind. |
| `level` | integer |  | 0-8 (indent level). |
| `paragraphs` | array of string | yes | Paragraph ids or numbers. |
| `start` | integer |  | First number. |
| `text` | string |  | Number format like "%1)" or "(%1)". |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `set_list_level`

Indent or outdent list items: change the list level (Tab / Shift+Tab in a bulleted or numbered list).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `level` | integer | yes | 0-8 |
| `paragraphs` | array of string | yes | Paragraph ids or numbers. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `remove_list`

Remove bullets or numbering from paragraphs, keeping the text.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraphs` | array of string | yes | Paragraph ids or numbers. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `restart_numbering`

Restart a numbered list at a paragraph, from 1 or any number (Set Numbering Value).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `start` | integer |  | Default 1. |

## Track changes

### `set_track_changes`

Turn Track Changes on or off for the document (Review > Track Changes).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author name for subsequent changes. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `on` | boolean | yes |  |

### `list_revisions`

Every tracked change: id, type, author, date, text, paragraph.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Filter by author. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `type` | string |  | Filter by type (insertion, deletion, run_format, ...). |

### `accept_revisions`

Accept tracked changes: all, by ids, or by author.

| Argument | Type | Required | Description |
|---|---|---|---|
| `by_author` | string |  | Only this author's changes. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `ids` | array of string |  | Revision ids (from list_revisions). Omit for all. |

### `reject_revisions`

Reject tracked changes: all, by ids, or by author.

| Argument | Type | Required | Description |
|---|---|---|---|
| `by_author` | string |  | Only this author's changes. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `ids` | array of string |  | Ids. |

### `compare_documents`

Review > Compare: build a new document showing `revised` as tracked changes against `original`, saved to `output`.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author shown for the changes. |
| `original` | string | yes | Path of the original .docx. |
| `output` | string | yes | Where to write the comparison. |
| `revised` | string | yes | Path of the revised .docx. |

## Comments

### `add_comment`

Comment on text in a paragraph (match or start/end; default the whole paragraph).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `end` | integer |  | End character offset. |
| `end_paragraph` | string |  | For a range spanning paragraphs: the paragraph where it ends (end = offset there). |
| `initials` | string |  |  |
| `match` | string |  | Exact text to act on. |
| `occurrence` | integer |  | Which occurrence of match (default 1). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `start` | integer |  | Start character offset. |
| `text` | string | yes | Comment text (\n for new paragraphs). |

### `list_comments`

All comments with author, date, text, the commented text, thread parent and resolved state.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `include_resolved` | boolean |  | Default true. |

### `reply_comment`

Reply to a comment (threaded like Word's Reply).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `comment_id` | string | yes | Comment id from list_comments. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `text` | string | yes | The text. |

### `resolve_comment`

Mark a comment thread resolved (or reopen it).

| Argument | Type | Required | Description |
|---|---|---|---|
| `comment_id` | string | yes | Comment id from list_comments. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `resolved` | boolean |  | Default true. |

### `edit_comment`

Change a comment's text.

| Argument | Type | Required | Description |
|---|---|---|---|
| `comment_id` | string | yes | Comment id from list_comments. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `text` | string | yes | The text. |

### `delete_comment`

Delete a comment and its replies; or all comments (optionally by author).

| Argument | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  |  |
| `by_author` | string |  | Only this author's changes. |
| `comment_id` | string |  | Omit with all=true to delete every comment. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

## Tables

### `insert_table`

Insert a table after/before a paragraph. rows = array of arrays of cell text. Default style Table Grid, autofit to window.

| Argument | Type | Required | Description |
|---|---|---|---|
| `alignment` | `left` \\| `center` \\| `right` |  |  |
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `header_row` | boolean |  | Default true. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `repeat_header` | boolean |  | Repeat header row on each page. |
| `rows` | array of any | yes | Rows: [["Item","Price"],["Setup","500"]], or objects [{"Item":"Setup","Price":500}] (keys become the header row). |
| `style` | string |  | Table style, e.g. TableGrid, "Grid Table 4 Accent 1". |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `where` | `after` \\| `before` |  | after or before. |
| `widths` | array of number |  | Column widths (pt or "1.5in"). |

### `read_table`

A table's cells (text), spans and style. Tables are numbered 1.. in document order.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `table` | integer | yes | 1-based |

### `set_cell`

Replace a cell's text and/or format it. row/col are 1-based.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `col` | integer | yes | Column number (1-based). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object |  | Cell: shading, vertical_align (top\|center\|bottom), width, borders, text_direction, no_wrap, paragraph{..}, run{..} |
| `row` | integer | yes | Row number (1-based). |
| `table` | integer | yes | Table number (1 = first table). |
| `text` | string |  | The text. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `table_rows`

Add a row above or below, or delete rows, in a table. row is 1-based.

| Argument | Type | Required | Description |
|---|---|---|---|
| `action` | `insert_above` \\| `insert_below` \\| `delete` | yes |  |
| `author` | string |  | Author of this change. |
| `cells` | array of string |  | Texts for the new row. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `row` | integer | yes | Row number (1-based). |
| `table` | integer | yes | Table number (1 = first table). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `table_columns`

Add or delete table columns (insert_left, insert_right, delete). col is 1-based.

| Argument | Type | Required | Description |
|---|---|---|---|
| `action` | `insert_left` \\| `insert_right` \\| `delete` | yes |  |
| `author` | string |  | Author of this change. |
| `col` | integer | yes | Column number (1-based). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `table` | integer | yes | Table number (1 = first table). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `merge_cells`

Merge table cells: a rectangle of cells becomes one (1-based, inclusive).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `from_col` | integer | yes |  |
| `from_row` | integer | yes |  |
| `table` | integer | yes | Table number (1 = first table). |
| `to_col` | integer | yes |  |
| `to_row` | integer | yes |  |

### `format_table`

style, alignment, width (pt|"50%"|auto), borders ("all"|"none"|{top,insideH,..}), indent, layout (fixed|autofit), cell_margins, header_row, first_column, last_row, last_column, banded_rows, banded_columns, repeat_header, row_height, column_widths.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object | yes |  |
| `table` | integer | yes | Table number (1 = first table). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `delete_table`

Delete a whole table from the document.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `table` | integer | yes | Table number (1 = first table). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

## Insert

### `insert_picture`

Insert an inline picture from a file (PNG, JPEG, GIF, BMP, TIFF, SVG, EMF, WMF) or base64 data. Size in points; give width or height to keep the aspect ratio. Too-wide pictures are scaled to the text width like Word.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `alt_text` | string |  | Alt text for accessibility. |
| `author` | string |  | Author of this change. |
| `data_base64` | string |  | Image bytes (alternative to file). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `file` | string |  | Image file path. |
| `height` | number |  | pt |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `width` | number |  | pt |

### `insert_break`

Page break, column break, line break, or a section break after a paragraph.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | For page/column/line breaks. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `type` | `page` \\| `column` \\| `line` \\| `section_next_page` \\| `section_continuous` \\| `section_even_page` \\| `section_odd_page` | yes |  |

### `insert_hyperlink`

Link text to a URL or to a bookmark ("#name"). Wrap existing text (match) or insert new link text.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `match` | string |  | Existing text to link. |
| `occurrence` | integer |  | Which occurrence of match (default 1). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Where to insert new text. |
| `text` | string |  | New link text to insert. |
| `tooltip` | string |  |  |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `url` | string | yes | https://..., mailto:..., or #bookmark |

### `list_hyperlinks`

List the links in the document: their text, URL or bookmark target, and paragraph.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `remove_hyperlinks`

Remove hyperlinks (keeping the text). Filter by target substring.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `target_contains` | string |  |  |

### `add_bookmark`

Add a bookmark on text (match/start/end) or at a point, for links and cross-references.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `end` | integer |  | End character offset. |
| `match` | string |  | Exact text to act on. |
| `name` | string | yes | Letters, digits, _; starts with a letter; max 40. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `start` | integer |  | Start character offset. |

### `list_bookmarks`

List bookmarks with the text they mark. include_hidden adds Word's hidden _Toc/_Ref ones.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `include_hidden` | boolean |  | Include _Toc/_Ref/_GoBack bookmarks. |

### `delete_bookmark`

Remove a bookmark (the text stays).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `name` | string | yes | Name. |

### `insert_note`

Insert a footnote or endnote reference at a position with the note text.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Place the reference mark after this text. |
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `footnote` \\| `endnote` |  | Kind. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `text` | string | yes | The text. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `list_notes`

List footnotes and endnotes with their text and the paragraph they belong to.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `delete_note`

Delete a footnote or endnote and its reference mark.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `id` | string | yes | Id. |
| `kind` | `footnote` \\| `endnote` | yes | Kind. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `insert_field`

Insert a Word field: PAGE, NUMPAGES, DATE \@ "MMMM d, yyyy", TIME, AUTHOR, TITLE, FILENAME, REF bookmark \h, PAGEREF bookmark \h, SEQ Figure \* ARABIC, STYLEREF "Heading 1", NUMWORDS, MERGEFIELD Name, IF ...

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `author` | string |  | Author of this change. |
| `code` | string | yes | Field instruction. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `result` | string |  | Cached display text (Word updates it). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `list_fields`

List fields (PAGE, DATE, TOC, REF, MERGEFIELD...) with their codes and current results.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `update_fields`

Update TOC entries, SEQ numbering and REF results now; page numbers are refreshed by Word on open (updateFields is set).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `insert_toc`

Insert a table of contents (References > Table of Contents) before a paragraph, built from headings.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `from_level` | integer |  | Default 1. |
| `hyperlinks` | boolean |  | Default true. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `title` | string |  | Heading above the TOC (default "Contents"; empty for none). |
| `to_level` | integer |  | Default 3. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `insert_equation`

Insert an equation from LaTeX-like input: x^2, a_i, \frac{a}{b}, \sqrt{x}, \sum_{i=1}^{n}, \int, Greek letters.

| Argument | Type | Required | Description |
|---|---|---|---|
| `display` | boolean |  | Own line (display) vs inline. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `equation` | string | yes |  |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `insert_caption`

Add a numbered caption (References > Insert Caption): "Figure 1: text", "Table 2: text". Numbers are real SEQ fields, so they renumber when captions are added or moved. Put it after a picture's paragraph, or before a table (table = n).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `label` | string |  | Figure (default), Table, Equation, or your own one-word label. |
| `paragraph` | string |  | Paragraph to put the caption next to. |
| `separator` | string |  | Between number and text (default ": "). |
| `table` | integer |  | Or a table number (1-based) to caption. |
| `text` | string |  | Caption text after the number. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `where` | `after` \\| `before` |  | Default: before a table, after anything else. |

### `insert_cross_reference`

Insert a cross-reference (References > Cross-reference) to a heading, caption, bookmark or note: "see Figure 2", "as described in Section 3", "on page 5". It's a REF/PAGEREF/NOTEREF field that stays right when the document changes. Targets come from list_reference_targets.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `link` | boolean |  | Ctrl+click jumps to the target (default true). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `show` | `text` \\| `number` \\| `label_number` \\| `page` \\| `above_below` |  | What to show: the text, its number, "Figure 2" (label_number), the page, or above/below. Default text. |
| `target` | string | yes | Heading or caption paragraph id, bookmark name, or footnote:N / endnote:N. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `list_reference_targets`

What a cross-reference can point at: headings, figure/table captions, bookmarks, footnotes and endnotes, with the target id to pass to insert_cross_reference.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `add_content_control`

Wrap text in (or insert) a content control: richText, text, checkbox, dropDown, comboBox, date, picture. Used for forms and templates.

| Argument | Type | Required | Description |
|---|---|---|---|
| `checked` | boolean |  |  |
| `date_format` | string |  | e.g. M/d/yyyy |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `items` | array of string |  | Choices for dropDown/comboBox. |
| `lock_content` | boolean |  |  |
| `lock_delete` | boolean |  |  |
| `match` | string |  | Text to wrap (omit to insert an empty control at position). |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `placeholder` | string |  |  |
| `position` | integer |  | Character offset to insert at (default end). |
| `tag` | string |  |  |
| `title` | string |  |  |
| `type` | `richText` \\| `text` \\| `checkbox` \\| `dropDown` \\| `comboBox` \\| `date` \\| `picture` |  |  |

### `list_content_controls`

List content controls (fillable fields, checkboxes, dropdowns, dates) with tag, title, type and value.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `set_content_control`

Set a content control's value (text, checkbox true/false, drop-down choice, date YYYY-MM-DD).

| Argument | Type | Required | Description |
|---|---|---|---|
| `control` | string | yes | id, tag or title |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `value` | any | yes | string or boolean |

## Page layout

### `get_page_setup`

Sections with page size, orientation, margins, columns, header/footer parts.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `set_page_setup`

Set page layout of a section (1-based, default all): orientation, paper (letter|a4|legal|a3|a5|b5|executive|tabloid), page_width/page_height, margins {top,bottom,left,right,header,footer,gutter} (pt or "1in"), columns, column_space, vertical_align, line_numbers, page_numbers {start, format}, title_page (different first page), type (section start).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `even_odd_headers` | boolean |  |  |
| `section` | integer |  | Omit for all sections. |
| `setup` | object | yes |  |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `set_header_footer`

Set a header or footer's text for a section. Use {PAGE}, {NUMPAGES}, {DATE}, {SECTIONPAGES} placeholders for fields. which: default (all pages), first (first page, turns on Different First Page), even (even pages).

| Argument | Type | Required | Description |
|---|---|---|---|
| `alignment` | `left` \\| `center` \\| `right` |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `header` \\| `footer` | yes | Kind. |
| `section` | integer |  | Default 1. |
| `text` | string | yes | Lines separated by \n. |
| `which` | `default` \\| `first` \\| `even` |  |  |

### `remove_header_footer`

Remove a header or footer (default, first page or even pages) from a section.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `header` \\| `footer` | yes | Kind. |
| `section` | integer |  |  |
| `which` | `default` \\| `first` \\| `even` |  |  |

### `read_header_footer`

Read a header or footer: its paragraphs with ids you can edit (paragraph = "part#id").

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `header` \\| `footer` | yes | Kind. |
| `section` | integer |  |  |
| `which` | `default` \\| `first` \\| `even` |  |  |

## File

### `get_properties`

Core (title, author, ...), custom, and application properties.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `set_properties`

Set core properties (title, subject, creator, keywords, description, category, content_status, language, ...) and/or custom properties (name -> string/number/bool; null removes).

| Argument | Type | Required | Description |
|---|---|---|---|
| `core` | object |  |  |
| `custom` | object |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `get_settings`

Document settings: track changes, protection, compatibility mode, tab stops, zoom, ...

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `set_settings`

track_changes, update_fields_on_open, even_odd_headers, mirror_margins, gutter_at_top, default_tab_stop (pt), zoom, show_markup, show_formatting_changes, do_not_track_moves, do_not_track_formatting, auto_hyphenation, embed_true_type_fonts, hide_spelling_errors, hide_grammar_errors, compatibility_mode (15 = Word 2013+).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `settings` | object | yes |  |

### `protect_document`

Restrict editing (Review > Restrict Editing): readOnly, comments, trackedChanges, forms. Optional password hashed exactly as Word does (SHA-512, 100000 spins).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `mode` | `readOnly` \\| `comments` \\| `trackedChanges` \\| `forms` | yes |  |
| `password` | string |  | Password. |

### `unprotect_document`

Remove editing restrictions (password required if one was set).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `password` | string |  | Password. |

## Expert

### `validate_document`

Check every part against the full OOXML schema (Office 2007-365 extensions) and the package's relationships.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `get_xml`

Inspect the raw XML of a part or one paragraph (for diagnosing unusual content).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `max_chars` | integer |  | Default 20000. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `part` | string |  | Part name, e.g. /word/styles.xml (default main document). |

### `set_paragraph_xml`

Expert escape hatch: replace a paragraph with raw WordprocessingML (one or more w:p/w:tbl). Prefixes w, r, a, m, wp, w14... are predeclared. The result is schema-validated; invalid XML is rejected.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `xml` | string | yes |  |

## Finding tools

### `help`

Help for Word documents (docx files): lists everything this can do, by task, and which tool does it. Open, create, read, write and edit text and paragraphs; format font, size, bold, headings, styles; tables, rows, cells; pictures; lists and bullets; headers, footers, page numbers, margins; comments; track changes; footnotes; links; table of contents. Give a topic or tool name to see its arguments. Call this first when the Word tool you need isn't in your list, then use run_tool.

| Argument | Type | Required | Description |
|---|---|---|---|
| `topic` | string |  | A task ("tables", "add a comment") or a tool name. Omit for everything. |

### `run_tool`

Run any Word document tool by name, including ones your client didn't list (find them with help). Same arguments as calling the tool directly: edit, format, tables, comments, track changes, headers, anything.

| Argument | Type | Required | Description |
|---|---|---|---|
| `arguments` | object |  | That tool's arguments. |
| `tool` | string | yes | Tool name from help, e.g. insert_table. |


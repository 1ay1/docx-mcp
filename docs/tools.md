# Tool reference

Generated from `docx-mcp --list-tools` by `tools/gen_tool_docs.py`. 121 tools.

Every tool that edits takes `doc` (a handle or a file path), and most take `track` (record as a tracked change) and `author`. Paragraphs are addressed by the `id` that `read_document` returns.

## Contents

- [Documents](#documents): `open_document`, `create_document`, `save_document`, `close_document`, `list_open_documents`, `undo`, `batch`
- [Reading](#reading): `read_document`, `get_text`, `get_outline`, `get_paragraph`, `find_text`, `get_statistics`
- [Editing text](#editing-text): `insert_text`, `replace_text`, `delete_text`, `find_replace`, `insert_paragraph`, `insert_markdown`, `insert_html`, `delete_paragraph`, `split_paragraph`, `merge_paragraphs`, `move_paragraphs`
- [Formatting](#formatting): `format_text`, `format_paragraph`, `apply_style`, `clear_formatting`, `drop_cap`
- [Styles](#styles): `list_styles`, `get_style`, `modify_style`, `delete_style`, `import_styles`
- [Lists](#lists): `apply_list`, `set_list_level`, `remove_list`, `restart_numbering`, `define_list`, `list_lists`
- [Track changes](#track-changes): `set_track_changes`, `list_revisions`, `accept_revisions`, `reject_revisions`, `compare_documents`
- [Comments](#comments): `add_comment`, `list_comments`, `reply_comment`, `resolve_comment`, `edit_comment`, `delete_comment`
- [Tables](#tables): `insert_table`, `read_table`, `set_cell`, `table_rows`, `table_columns`, `merge_cells`, `split_cell`, `format_table`, `sort_table`, `convert_table`, `delete_table`
- [Insert](#insert): `insert_picture`, `list_pictures`, `format_picture`, `delete_picture`, `insert_text_box`, `insert_shape`, `insert_chart`, `list_charts`, `set_chart_data`, `insert_break`, `insert_hyperlink`, `list_hyperlinks`, `remove_hyperlinks`, `add_bookmark`, `list_bookmarks`, `delete_bookmark`, `insert_note`, `list_notes`, `delete_note`, `insert_field`, `list_fields`, `update_fields`, `unlink_fields`, `insert_toc`, `mark_index_entry`, `insert_index`, `insert_equation`, `insert_caption`, `insert_cross_reference`, `list_reference_targets`, `add_source`, `insert_citation`, `insert_bibliography`, `add_content_control`, `list_content_controls`, `set_content_control`
- [Mailings](#mailings): `mail_merge`, `document_variables`
- [Page layout](#page-layout): `get_page_setup`, `set_page_setup`, `set_header_footer`, `remove_header_footer`, `read_header_footer`, `set_watermark`, `theme`
- [File](#file): `get_properties`, `set_properties`, `get_settings`, `set_settings`, `protect_document`, `unprotect_document`
- [Expert](#expert): `validate_document`, `inspect_document`, `get_xml`, `set_paragraph_xml`
- [Seeing the page](#seeing-the-page): `render_pages`, `export_pdf`
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

### `batch`

Run a list of tool calls on one document in order, as one change: if any step fails, none of them apply (and undo reverts the whole batch). Each step is {tool, arguments}; `doc` is filled in for you. Results come back per step. Saves round trips when making many edits.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `steps` | array of object | yes | The calls, in order. |
| `stop_on_error` | boolean |  | Default true: roll back everything on the first error. false: keep going and report errors per step. |

## Reading

### `read_document`

Read paragraphs with their ids, styles, list labels, table positions and revision/comment markers. Paginate with offset/limit. view=original shows the text before tracked changes. pages=true adds the page each paragraph is on (laid out through LibreOffice, cached until the next edit).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `limit` | integer |  | Max paragraphs (default 200). |
| `offset` | integer |  | 1-based first paragraph (negative = from the end). |
| `pages` | boolean |  | Add the page number of each paragraph (needs LibreOffice). |
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

### `insert_markdown`

Write Markdown as real Word content: # headings (Heading 1-6), paragraphs with **bold**, *italic*, `code`, ~~strike~~ and [links](url), - bullets and 1. numbered lists (indent 2 spaces to nest), > quotes, ``` code blocks, | tables |, --- rules, \pagebreak. The fastest way to write a whole document. Goes after `paragraph` (or the end).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `markdown` | string | yes | Markdown text. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `table` | integer |  | Anchor on this table (1-based) instead of a paragraph. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `where` | `after` \\| `before` |  | Default after. |

### `insert_html`

Paste HTML as real Word content (Keep Source Formatting): h1-h6, p, b/i/u/s, sup/sub, code, mark, span styles (color, background, font-size, font-family, font-weight), links, ul/ol nesting, blockquote, pre, tables (th, colspan, cell background), hr, br, img (data: URIs or local files). From a browser, email or a generator. html_file reads it from disk. Goes after `paragraph` (or the end).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `html` | string |  | HTML text. |
| `html_file` | string |  | Read the HTML from this file instead. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `table` | integer |  | Anchor on this table (1-based) instead of a paragraph. |
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

Cut paragraphs and paste them elsewhere (before/after a target). With Track Changes this is a real Word move: shown as moved from / moved to, accepted or rejected as one.

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

Change the font, size, color, bold, italic, underline, highlight of text in a paragraph (match or start/end; default the whole paragraph).

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

### `drop_cap`

Make the first letter of a paragraph a large drop cap (Insert > Drop Cap). kind: drop (default), margin, or none to remove it.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `drop` \\| `margin` \\| `none` |  | Default drop. |
| `letters` | integer |  | Default 1. |
| `lines` | integer |  | Lines to drop, default 3. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
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

Create a new style or modify one (Modify Style dialog). Changes apply everywhere the style is used. Table styles also take table (borders, shading, cell_margins, band_size) and conditional formats per part: header_row, total_row, first_column, last_column, banded_rows, banded_columns: {run, paragraph, cell: {shading, borders}}.

| Argument | Type | Required | Description |
|---|---|---|---|
| `based_on` | string |  |  |
| `conditional` | object |  | Table styles: {"header_row": {"run": {"bold": true, "color": "FFFFFF"}, "cell": {"shading": "1F4E79"}}, "banded_rows": {"cell": {"shading": "DDEBF7"}}} |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `hidden` | boolean |  |  |
| `name` | string |  | Name. |
| `next` | string |  | Style for the next paragraph. |
| `paragraph` | object |  | Paragraph formatting (Word names). Lengths in points or with a unit ("0.5in", "1cm"). null removes. More keys: line_rule, first_line_indent, hanging_indent, keep_together, page_break_before, widow_control, outline_level, shading, borders, tabs, bidi. |
| `quick_style` | boolean |  | Show in the Styles gallery. |
| `run` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `style` | string |  | Style id or name; a new id is derived from name when creating. |
| `table` | object |  | Table styles: whole-table borders, shading, cell_margins, alignment, band_size. |
| `type` | `paragraph` \\| `character` \\| `table` \\| `numbering` |  | For new styles. |

### `delete_style`

Delete a style; text using it reverts to Normal / Default Paragraph Font.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `style` | string | yes | Style name or id. |

### `import_styles`

Copy styles from another .docx or .dotx (Organizer / a template's look): named styles, or all. The styles they are based on and their list numbering come along. overwrite replaces styles with the same id (default true); defaults also copies the document defaults.

| Argument | Type | Required | Description |
|---|---|---|---|
| `defaults` | boolean |  | Also copy the document defaults (docDefaults). Default false. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `from` | string | yes | Path of the template or document (or an open doc handle). |
| `overwrite` | boolean |  | Replace existing styles with the same id. Default true. |
| `styles` | array of string |  | Style ids or names. Omit for every style. |

## Lists

### `apply_list`

Make paragraphs a bulleted or numbered list (Word's default list definitions). Continues an adjacent list of the same kind.

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `bullet` | string |  | Bullet character. |
| `continue_previous` | boolean |  | Default true. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kind` | `bullet` \\| `number` \\| `lowerLetter` \\| `upperLetter` \\| `lowerRoman` \\| `upperRoman` |  | Kind. |
| `level` | integer |  | 0-8 (indent level). |
| `num_id` | integer |  | Use this existing list (from list_lists or define_list); kind is then ignored. |
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

### `define_list`

Define New Multilevel List: create a list definition and optionally apply it. preset: numbered (1. 1.1. 1.1.1.), legal, outline (I. A. 1. a) (1)), headings (numbers Heading 1-9 as 1 / 1.1 / 1.1.1), chapters (Chapter 1 / Section 1.1 on Heading 1-3), bullets, checklist. levels overrides levels from the top: format (decimal, lowerLetter, upperRoman, bullet, ...), text (%1 is level 1's number: "%1.%2)"), start, style (link a heading style), indent, hanging, after (tab|space|nothing), bullet, legal. paragraphs applies it at level.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `level` | integer |  | 0-8 for paragraphs (default 0). |
| `levels` | array of object |  | Per-level settings from level 1 down, e.g. [{"format": "upperRoman"}, {"text": "%1.%2"}]. |
| `name` | string |  | Name shown in Word's list library. |
| `paragraphs` | array of string |  | Paragraphs to put in the new list. |
| `preset` | `numbered` \\| `legal` \\| `outline` \\| `headings` \\| `chapters` \\| `bullets` \\| `checklist` |  | Default numbered. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `list_lists`

The document's list definitions: num_id, each level's format and number text, linked styles, and how many paragraphs use it. Pass num_id to apply_list to continue one.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

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

### `split_cell`

Split table cells: undo a merge at a cell, or split a plain cell into several side by side (cols). row/col are 1-based.

| Argument | Type | Required | Description |
|---|---|---|---|
| `col` | integer | yes | Column number (1-based). |
| `cols` | integer |  | For a plain cell: how many cells to split it into (default 2). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `row` | integer | yes | Row number (1-based). |
| `table` | integer | yes | Table number (1 = first table). |

### `format_table`

style, alignment, width (pt|"50%"|auto), borders ("all"|"none"|{top,insideH,..}), indent, layout (fixed|autofit), cell_margins, header_row, first_column, total_row, last_column, banded_rows, banded_columns, repeat_header, row_height, exact_row_height, column_widths, cant_split, title/description (alt text), rtl, float (text wraps around: {x: pt|left|center|right, y: pt|top|bottom, horizontal_relative: margin|page|text, vertical_relative, distance, overlap}; null = inline).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `format` | object | yes |  |
| `table` | integer | yes | Table number (1 = first table). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `sort_table`

Sort a table's rows by a column (Table > Sort). The header row stays on top. type: text, number or date.

| Argument | Type | Required | Description |
|---|---|---|---|
| `col` | integer | yes | Column to sort by (1-based). |
| `descending` | boolean |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `header` | boolean |  | First row is a header (default true). |
| `table` | integer | yes | Table number (1 = first table). |
| `type` | `text` \\| `number` \\| `date` |  | Default text. |

### `convert_table`

Convert Text to Table (paragraphs -> rows, cells split on separator) or Table to Text (rows -> paragraphs).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `header_row` | boolean |  | to table: first row is a header. |
| `paragraphs` | array of string |  | to table: the paragraphs, in order. |
| `separator` | string |  | tab (default), comma, semicolon or any text. |
| `style` | string |  | to table: table style, e.g. Grid Table 4 Accent 1. |
| `table` | integer |  | to text: the table. |
| `to` | `table` \\| `text` | yes |  |
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

Insert a picture or image (photo, logo, chart image) from a file (PNG, JPEG, GIF, BMP, TIFF, SVG, EMF, WMF) or base64 data. Size in points; give width or height to keep the aspect ratio. Too-wide pictures are scaled to the text width like Word. layout makes it float with text wrapping.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `alt_text` | string |  | Alt text for accessibility. |
| `author` | string |  | Author of this change. |
| `data_base64` | string |  | Image bytes (alternative to file). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `file` | string |  | Image file path. |
| `height` | number |  | pt |
| `layout` | any |  | Layout Options. "inline" (default) flows with the text; "square", "tight", "through", "top_bottom" wrap text around it; "behind" / "in_front" float over the text. Or an object: {wrap, horizontal: left\|center\|right, vertical: top\|center\|bottom, x, y (points or "1in"), horizontal_from: margin\|page\|column\|character, vertical_from: margin\|page\|paragraph\|line, distance (gap to text, points)}. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `width` | number |  | pt |

### `list_pictures`

List the pictures in the document: number, size in points, layout (inline or how text wraps), alt text and the paragraph each sits in.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `format_picture`

Resize a picture, change how text wraps around it (Layout Options), move it, or set its alt text. Give width or height alone to keep the aspect ratio.

| Argument | Type | Required | Description |
|---|---|---|---|
| `alt_text` | string |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `height` | number |  | pt |
| `layout` | any |  | Layout Options. "inline" (default) flows with the text; "square", "tight", "through", "top_bottom" wrap text around it; "behind" / "in_front" float over the text. Or an object: {wrap, horizontal: left\|center\|right, vertical: top\|center\|bottom, x, y (points or "1in"), horizontal_from: margin\|page\|column\|character, vertical_from: margin\|page\|paragraph\|line, distance (gap to text, points)}. |
| `picture` | integer | yes | From list_pictures (1-based). |
| `width` | number |  | pt |

### `delete_picture`

Delete a picture (number from list_pictures).

| Argument | Type | Required | Description |
|---|---|---|---|
| `author` | string |  | Author of this change. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `picture` | integer | yes |  |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `insert_text_box`

Insert a text box (Insert > Text Box): a bordered box of text that floats with text wrapped around it, for callouts, sidebars and pull quotes. Lines in `text` become paragraphs; edit them later by the returned ids.

| Argument | Type | Required | Description |
|---|---|---|---|
| `alignment` | `left` \\| `center` \\| `right` |  |  |
| `border` | any |  | Outline color, or false for none. Object {color, size} also works. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `fill` | string |  | Background, RRGGBB or a name. |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `height` | number |  | pt, default fits the text |
| `layout` | any |  | Layout Options. "inline" (default) flows with the text; "square", "tight", "through", "top_bottom" wrap text around it; "behind" / "in_front" float over the text. Or an object: {wrap, horizontal: left\|center\|right, vertical: top\|center\|bottom, x, y (points or "1in"), horizontal_from: margin\|page\|column\|character, vertical_from: margin\|page\|paragraph\|line, distance (gap to text, points)}. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `shape` | string |  | rect (default), rounded, or any insert_shape shape. |
| `text` | string | yes | Text; \n separates paragraphs. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `width` | number |  | pt, default 216 (3in) |

### `insert_shape`

Insert > Shapes: a floating shape anchored in a paragraph. shape: rect, rounded, ellipse, triangle, diamond, arrow (right), left_arrow, up_arrow, down_arrow, star, callout, speech, hexagon, chevron, heart, cloud, banner, cylinder, process, decision, terminator, line, or any DrawingML preset (flowChartDecision, star8, ...). Optional text sits in the middle. Lines take start_arrow / end_arrow. layout positions it (default: in front of text at the paragraph).

| Argument | Type | Required | Description |
|---|---|---|---|
| `border` | any |  | Outline color, or false. Object {color, size, dash: dash\|dot\|dash_dot\|long_dash}. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `end_arrow` | `none` \\| `triangle` \\| `stealth` \\| `diamond` \\| `oval` \\| `arrow` |  | Lines: arrowhead at the end. |
| `fill` | string |  | RRGGBB, a name, or none. Default the theme accent. |
| `flip_horizontal` | boolean |  |  |
| `flip_vertical` | boolean |  |  |
| `format` | object |  | Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow. |
| `height` | number |  | pt, default fits the shape |
| `layout` | any |  | Layout Options. "inline" (default) flows with the text; "square", "tight", "through", "top_bottom" wrap text around it; "behind" / "in_front" float over the text. Or an object: {wrap, horizontal: left\|center\|right, vertical: top\|center\|bottom, x, y (points or "1in"), horizontal_from: margin\|page\|column\|character, vertical_from: margin\|page\|paragraph\|line, distance (gap to text, points)}. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `rotation` | number |  | Degrees clockwise. |
| `shape` | string |  | Shape name; default rect. |
| `start_arrow` | `none` \\| `triangle` \\| `stealth` \\| `diamond` \\| `oval` \\| `arrow` |  | Lines: arrowhead at the start. |
| `text` | string |  | Text inside the shape. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `width` | number |  | pt, default 144 |

### `insert_chart`

Insert > Chart from data: type column, bar (horizontal), line, pie, doughnut, area or scatter. categories label the x axis; series are [{name, values, color}]. grouping clustered|stacked|percent; title, x_axis_title, y_axis_title, legend, legend_position, data_labels, smooth (lines), markers. Width/height in points (default 432 x 260).

| Argument | Type | Required | Description |
|---|---|---|---|
| `alt_text` | string |  | Defaults to the title. |
| `categories` | array of string |  | Category labels, e.g. ["Q1", "Q2"]. |
| `data_labels` | boolean |  | Show values (percent on pies). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `grouping` | `clustered` \\| `stacked` \\| `percent` |  | Bars, columns, lines, areas. |
| `height` | number |  | pt |
| `legend` | boolean |  | Default true. |
| `legend_position` | `bottom` \\| `top` \\| `left` \\| `right` |  |  |
| `markers` | boolean |  |  |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `series` | array of object | yes | [{"name": "Sales", "values": [10, 20], "color": "1F4E79"}]; scatter series take x too. |
| `smooth` | boolean |  |  |
| `title` | string |  |  |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |
| `type` | string |  | column (default), bar, line, pie, doughnut, area, scatter. |
| `width` | number |  | pt |
| `x_axis_title` | string |  |  |
| `y_axis_title` | string |  |  |

### `list_charts`

List charts with their type, title, categories and series values (the data Word shows). Use the number with set_chart_data.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `set_chart_data`

Change a chart (number from list_charts): new series, categories, type, title, grouping... Anything not given keeps what the chart shows now.

| Argument | Type | Required | Description |
|---|---|---|---|
| `categories` | array of string |  |  |
| `chart` | integer | yes | 1-based. |
| `data_labels` | boolean |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `grouping` | `clustered` \\| `stacked` \\| `percent` |  |  |
| `legend` | boolean |  |  |
| `series` | array of object |  |  |
| `title` | string |  |  |
| `type` | string |  |  |
| `x_axis_title` | string |  |  |
| `y_axis_title` | string |  |  |

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

Insert a Word field: PAGE, NUMPAGES, DATE \@ "MMMM d, yyyy", TIME, AUTHOR, TITLE, FILENAME, REF bookmark \h, PAGEREF bookmark \h, SEQ Figure \* ARABIC, STYLEREF "Heading 1", NUMWORDS, MERGEFIELD Name, DOCPROPERTY Client, DOCVARIABLE Version, = SUM(ABOVE) \# "#,##0.00", IF { MERGEFIELD Total } > 100 "big" "small" ({ } nest fields). The result is computed right away when it can be.

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

Update Fields (F9): every field's result is computed now: TOC entries, SEQ numbers, REF, IF, = formulas, DATE, DOCPROPERTY, DOCVARIABLE, STYLEREF, ... With pages=true the document is laid out (LibreOffice) so PAGE, NUMPAGES, PAGEREF, TOC and index page numbers are real too; otherwise Word fills those in on open.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `pages` | boolean |  | Lay pages out for page numbers (needs LibreOffice). Default false. |
| `paragraph` | string |  | Only fields in this paragraph. |

### `unlink_fields`

Replace fields by their current results as plain text (Ctrl+Shift+F9). kinds limits it to some field types (e.g. ["MERGEFIELD", "IF"]); omit for every field. Run update_fields first if results are stale.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `kinds` | array of string |  | Field names to unlink, e.g. MERGEFIELD, DATE. Omit for all. |

### `insert_toc`

Insert a table of contents (References > Table of Contents) before a paragraph, built from headings. captions: "Figure" (or Table, ...) makes a Table of Figures from those captions instead.

| Argument | Type | Required | Description |
|---|---|---|---|
| `captions` | string |  | Caption label for a Table of Figures, e.g. Figure or Table. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `from_level` | integer |  | Default 1. |
| `hyperlinks` | boolean |  | Default true. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `title` | string |  | Heading above the TOC (default "Contents"; empty for none). |
| `to_level` | integer |  | Default 3. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `mark_index_entry`

Mark a word or phrase for the index (References > Mark Entry). Put it after `match` text (or at position). sub makes a sub-entry under term. Then insert_index builds the index.

| Argument | Type | Required | Description |
|---|---|---|---|
| `all` | boolean |  | Mark every paragraph that contains the text (Mark All). |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `match` | string |  | Mark this text (the entry goes right after it; default term). |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `sub` | string |  | Sub-entry under term. |
| `term` | string | yes | Index entry text. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `insert_index`

Insert the index (References > Insert Index) before a paragraph, from the entries marked with mark_index_entry. Page numbers fill in when Word opens the file or on render_pages/export_pdf.

| Argument | Type | Required | Description |
|---|---|---|---|
| `columns` | integer |  | 1-4, default 2. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
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

### `add_source`

References > Manage Sources: add a source (book, article, website, report, conference, chapter, ...) with title, authors (["Last, First"] or ["First Last"]), year, publisher, city, journal, volume, issue, pages, url. Returns its tag for insert_citation. Also list (list: true) or remove (remove: tag).

| Argument | Type | Required | Description |
|---|---|---|---|
| `authors` | array of string |  |  |
| `book_title` | string |  | For chapters. |
| `city` | string |  |  |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `doi` | string |  |  |
| `issue` | string |  |  |
| `journal` | string |  |  |
| `list` | boolean |  | List the sources instead. |
| `pages` | string |  |  |
| `publisher` | string |  |  |
| `remove` | string |  | Remove the source with this tag. |
| `site` | string |  | Website name. |
| `tag` | string |  | Default: author + year. |
| `title` | string |  |  |
| `type` | string |  | book (default), article, website, report, conference, chapter, magazine, thesis, ... |
| `url` | string |  |  |
| `volume` | string |  |  |
| `year` | string |  |  |

### `insert_citation`

Cite a source in the text (in-text citation by tag from add_source), at a position: (Smith, 2020) in APA, [1] in IEEE. pages adds a page locator. Then insert_bibliography lists the cited books and articles.

| Argument | Type | Required | Description |
|---|---|---|---|
| `after` | string |  | Insert after this text. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `pages` | string |  | e.g. 12 or 12-14. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `position` | integer |  | Character offset to insert at (default end). |
| `source` | string | yes | Source tag. |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

### `insert_bibliography`

Insert a bibliography (References > Bibliography) after a paragraph, listing every source. style switches the citation style for the whole document: APA, MLA, Chicago or IEEE; Word refreshes it the same way.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `style` | `APA` \\| `MLA` \\| `Chicago` \\| `IEEE` |  | Citation style. |
| `title` | string |  | Heading above it (default "References"; empty for none). |
| `track` | boolean |  | Record as a tracked change (default: the document's setting). |

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

## Mailings

### `mail_merge`

Mailings > Finish & Merge: fill the document's MERGEFIELDs (and IF, SKIPIF, NEXT, MERGEREC) from records and write the results. records is a list of objects (column -> value), or data_file is a .csv / .tsv / .json. With output_dir one file per record (name_field picks the file name); with output one document holding every record, a section each. Without either it returns the fields the document uses. Insert fields with insert_field code="MERGEFIELD Name".

| Argument | Type | Required | Description |
|---|---|---|---|
| `data_file` | string |  | A .csv, .tsv or .json data source instead of records. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `name_field` | string |  | Column used to name per-record files (default: numbered). |
| `output` | string |  | One merged .docx with every record. |
| `output_dir` | string |  | A folder for one .docx per record. |
| `records` | array of object |  | Records: [{"Name": "Ann", "City": "Paris"}, ...] |

### `document_variables`

Read or set document variables (w:docVars): named values DOCVARIABLE fields show. set: {name: value, ...}; null removes.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `set` | object |  | Variables to set: {"Version": "2.1"}; null removes one. |
| `update` | boolean |  | Refresh DOCVARIABLE fields after setting (default true). |

## Page layout

### `get_page_setup`

Sections with page size, orientation, margins, columns, header/footer parts.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |

### `set_page_setup`

Set page layout of a section (1-based, default all): orientation, paper (letter|a4|legal|a3|a5|b5|executive|tabloid), page_width/page_height, margins {top,bottom,left,right,header,footer,gutter} (pt or "1in"), columns, column_space, column_line, column_widths, page_borders, page_color, vertical_align, line_numbers, page_numbers {start, format}, title_page (different first page), type (section start).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `even_odd_headers` | boolean |  |  |
| `section` | integer |  | Omit for all sections. |
| `setup` | object | yes | Lengths in pt or with units ("1in", "2cm"). |
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

### `set_watermark`

Put a text watermark behind every page (Design > Watermark), e.g. DRAFT or CONFIDENTIAL, or a picture watermark (picture = image path or data: URI; washout fades it, scale is a percent, default fits the page). remove: true takes it off.

| Argument | Type | Required | Description |
|---|---|---|---|
| `color` | string |  | RRGGBB or a name. Default silver. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `font` | string |  | Default Calibri. |
| `layout` | `diagonal` \\| `horizontal` |  | Default diagonal. |
| `opacity` | number |  | 0-1, default 0.5. |
| `picture` | string |  | Picture watermark: an image path or data: URI. |
| `remove` | boolean |  | Remove the watermark instead. |
| `scale` | number |  | Picture watermarks: percent of its size (default fits the text width). |
| `size` | number |  | Points; default fits the page width. |
| `text` | string |  | Watermark text. |
| `washout` | boolean |  | Picture watermarks: fade it (default true). |

### `theme`

Design > Themes: read or change the theme's colors and fonts. Headings use the heading font, text the body font, and theme colors (accent1-6, dk2, lt2, hlink) feed styles, tables and charts. preset: Office, Office 2007, Office 2013, Blue, Blue II, Green, Red Orange, Violet, Marquee, Paper, Grayscale. colors: {"accent1": "1F4E79"}. Without changes it returns the current theme.

| Argument | Type | Required | Description |
|---|---|---|---|
| `body_font` | string |  | Minor (body) typeface. |
| `colors` | object |  | Theme color slots: dk1, lt1, dk2, lt2, accent1-6, hlink, folHlink -> RRGGBB. |
| `colors_preset` | string |  | Only the colors of a preset. |
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `fonts_preset` | string |  | Only the fonts: Office, Georgia, Garamond, Arial, ... |
| `heading_font` | string |  | Major (headings) typeface. |
| `name` | string |  | Theme name. |
| `preset` | string |  | Built-in theme colors (and fonts when it has them). |

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

### `inspect_document`

Check what the file carries besides its text before sharing it (File > Inspect Document): comments, tracked changes, personal info (author names), hidden text, custom XML, custom properties, watermarks. remove: a list of those (comments, revisions, personal_info, hidden_text, custom_xml, custom_properties, watermark) or ["all"] cleans them out; revisions are accepted.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `remove` | array of string |  | Categories to remove. |

### `get_xml`

Inspect the raw XML of a part or one paragraph (for diagnosing unusual content).

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `max_chars` | integer |  | Default 20000. |
| `paragraph` | string |  | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `part` | string |  | Part name, e.g. /word/styles.xml (default main document). |

### `set_paragraph_xml`

Expert escape hatch for things no other tool covers: replace a paragraph with raw WordprocessingML (one or more w:p/w:tbl). Prefixes w, r, a, m, wp, w14... are predeclared. Properties are put in the schema's order for you; anything still invalid is rejected with the reason. Prefer format_paragraph / format_text.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `paragraph` | string | yes | Paragraph `id` from read_document (e.g. "1A2B3C4D") or its number ("3"). |
| `xml` | string | yes |  |

## Seeing the page

### `render_pages`

Render pages of the document as images so you can see the layout: where pictures, tables and page breaks fall, how a heading looks. Returns PNG images (and the page count). Uses LibreOffice; unsaved edits are included.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `dpi` | integer |  | Resolution, 30-300 (default 80; 150 to read small text). |
| `page` | integer |  | First page (1-based, default 1). |
| `to_page` | integer |  | Last page (default = page; at most 20). |

### `export_pdf`

Export the document to PDF (File > Save As > PDF), with the current unsaved edits. Uses LibreOffice.

| Argument | Type | Required | Description |
|---|---|---|---|
| `doc` | string | yes | Handle from open_document (e.g. "doc1") or the .docx path. |
| `path` | string |  | Where to write the .pdf (default: next to the document). |

## Finding tools

### `help`

Help for Word documents (docx files): lists everything this can do, by task, and which tool does it. Open, create, read, write and edit text and paragraphs; format font, size, bold, headings, styles; tables, rows, cells; pictures; lists and bullets; headers, footers, page numbers, margins; comments; track changes; footnotes; links; table of contents. Give tool names or tasks (comma-separated) to see how to call them. Only needed when a Word tool isn't in your list; tools you can see, call directly.

| Argument | Type | Required | Description |
|---|---|---|---|
| `topic` | string |  | Tool names or tasks, comma-separated ("insert_table, format_text", "add a comment"). Omit for everything. |

### `run_tool`

Run any Word document tool by name, including ones your client didn't list (find them with help). Same arguments as calling the tool directly: edit, format, tables, comments, track changes, headers, anything.

| Argument | Type | Required | Description |
|---|---|---|---|
| `arguments` | object |  | That tool's arguments. |
| `tool` | string | yes | Tool name from help, e.g. insert_table. |


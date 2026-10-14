# Guide

How to use docx-mcp well, with examples. Arguments are shown as JSON, the
way an MCP client sends them.

## Opening and saving

```json
{"tool": "open_document", "arguments": {"path": "contract.docx", "author": "Alice"}}
```

This returns a handle (`"doc": "doc1"`) and a summary: paragraph count, words,
tables, comments, revisions, whether Track Changes is on, and protection. You
can pass the handle or the file path as `doc` to every other tool.

Nothing touches the disk until `save_document`. Saving refuses to write a file
with schema problems that your edits introduced. Problems that were already
in the file when you opened it don't block saving. `save_document` with a
`path` is Save As.

`undo` steps back through the last 30 changes.

New documents:

```json
{"tool": "create_document", "arguments": {"path": "memo.docx", "title": "Memo", "paper": "a4"}}
```

You get Word's own blank document: Calibri 11, the Office theme, Word's
spacing defaults.

## Finding your place

`read_document` lists paragraphs with:

- `id`: an 8-digit hex id (Word's `w14:paraId`). It stays stable while you
  edit, so use it to address paragraphs.
- `n`: the 1-based position. This works too, but it shifts when paragraphs are
  added or removed.
- `style`, `list` (the rendered number or bullet), and `in` (for example
  `table 1 r2c3` when the paragraph is inside a table).
- `marks`: tracked insertions and deletions, format changes and comment ids.

Pass `"runs": true` to get each run's text, character offsets and formatting.
Pass `"view": "original"` to see the text as it was before tracked changes.

`find_text` searches the body, headers, footers, footnotes and comments. It
returns `paragraph`, `start` and `end`, ready to use in an edit.

## Addressing text inside a paragraph

Every tool that works on part of a paragraph takes one of two forms:

- `match` (plus an optional `occurrence`): the text to act on. This is the
  easiest form and survives earlier edits.
- `start` / `end`: character offsets in the visible text. Negative numbers
  count from the end.

Insertion points take `position`, `after` or `before`.

The visible text follows Word's conventions. A tab is `\t`, a line break is
`\v`, a page break is `\f`, and pictures and other objects are `U+FFFC`.

## Track Changes

Turn Track Changes on for the whole document:

```json
{"tool": "set_track_changes", "arguments": {"doc": "doc1", "on": true, "author": "Bob"}}
```

From then on, every edit is recorded the way Word records it:

- Typing goes into `w:ins`.
- Deleting goes into `w:del`. Deleting your own tracked insertion just removes
  it. Deleting someone else's insertion nests a deletion inside it.
- Formatting changes go into `w:rPrChange` / `w:pPrChange`, with the original
  properties kept.
- New paragraphs, table rows, cells, lists and section breaks get the matching
  revision marks.

You can also pass `"track": true` or `"track": false` to override the setting
for one call.

Reviewing:

```json
{"tool": "list_revisions", "arguments": {"doc": "doc1"}}
{"tool": "accept_revisions", "arguments": {"doc": "doc1", "by_author": "Bob"}}
{"tool": "reject_revisions", "arguments": {"doc": "doc1", "ids": ["14", "15"]}}
```

`compare_documents` builds a third document that shows the differences
between two files as tracked changes. It works paragraph by paragraph and word
by word, including inside tables.

## Comments

```json
{"tool": "add_comment", "arguments": {"doc": "doc1", "paragraph": "1A2B3C4D", "match": "45 days", "text": "Why 45?"}}
{"tool": "reply_comment", "arguments": {"doc": "doc1", "comment_id": "7", "text": "Standard term.", "author": "Alice"}}
{"tool": "resolve_comment", "arguments": {"doc": "doc1", "comment_id": "7"}}
```

Comments are written the way Word 365 writes them: `comments.xml`,
`commentsExtended.xml` (threads and resolved state), `commentsIds.xml`,
`commentsExtensible.xml` and `people.xml`. Replies thread under the first
comment, like Word's Reply button.

## Formatting

Character formatting uses Word's names: `bold`, `italic`, `underline`, `size`
(points), `font`, `color`, `highlight`, and so on. Paragraph formatting is the
same idea: `alignment`, `space_after`, `line_spacing`, `indent_left`, and so
on.

Lengths are in points unless you add a unit: `"0.5in"`, `"1cm"`, `"12pt"`.
Setting a property to `null` removes it.

Styles beat direct formatting for anything repeated. `apply_style` accepts an
id or a name (`"Heading 1"`, `"Title"`, `"Quote"`). If the document doesn't
have that built-in style yet, Word's definition of it is added.
`modify_style` changes a style everywhere it is used.

## Tables

```json
{"tool": "insert_table", "arguments": {"doc": "doc1", "paragraph": "1A2B3C4D",
  "rows": [["Item", "Price"], ["Setup", "500"]], "style": "Grid Table 4 Accent 1"}}
```

Tables are numbered 1, 2, ... in document order. Rows and columns in
`set_cell`, `table_rows`, `table_columns` and `merge_cells` are 1-based.

## Headers, footers, fields

```json
{"tool": "set_header_footer", "arguments": {"doc": "doc1", "kind": "footer", "text": "Page {PAGE} of {NUMPAGES}", "alignment": "center"}}
```

`{PAGE}`, `{NUMPAGES}`, `{DATE}` and `{SECTIONPAGES}` become real fields.

`insert_toc` builds a table of contents from the headings. Page numbers need
layout, so the server sets Word to update fields when the file opens.

## When the tools are not enough

`get_xml` shows the raw XML of a part or a paragraph. `set_paragraph_xml`
replaces a paragraph with WordprocessingML you write yourself. The result is
validated, and the change is refused if it adds a schema problem.

`validate_document` runs the full validator at any time.

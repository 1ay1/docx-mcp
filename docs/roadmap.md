# Roadmap

Where docx-mcp is, what's missing, and the order we'll build it in. Every item
says what "done" means and how it gets tested. Nothing ships without passing
the Open XML SDK validator, the coverage driver and the fuzzer.

The goal: an agent can do anything in a .docx that a person can do in Word,
the file is exactly what Word itself would write, and the agent never needs
to touch raw XML to get there.

## Where we are (main, after v0.3.0)

123 tools cover Word's Home, Insert, Design, Layout, References, Mailings and Review
tabs: documents, text, paragraphs, styles, lists, tables, pictures, text
boxes, links, bookmarks, notes, fields, TOC, table of figures, index,
equations, content controls, sections, page borders and color, watermarks,
headers and footers, comments, tracked changes, compare, protection,
properties, settings and Inspect Document. `insert_markdown` writes a whole
document in one call and `batch` runs many edits as one change.

Every output is checked by Microsoft's Open XML SDK validator in CI: 564
corpus files, 2,256 edited/accepted/rejected outputs, all 106 tools tracked
and untracked, and a fuzzer chaining random tool calls. 0 errors.

## Phase 0: make the tools reachable

**Problem.** A model working through agentty listed these as missing:
reading a document, creating one, inserting and deleting paragraphs, tables,
pictures, comments, accepting changes, headers, page setup, styles, lists,
footnotes and protection. All of them already exist. The model saw 17 of
the 85 tools and had no way to find the rest.

agentty sends the model the 16 MCP tools ranked best for the request, plus
pinned ones. It only adds `mcp_search_tools` / `mcp_call` (the way to reach
the rest) when a server has more tools than that, and only on paths that
allow it. On "what can you do with Word docs?" the ranking picked
`set_list_level` and `remove_hyperlinks` over `read_document` and
`create_document`. The model then concluded the rest didn't exist.

Any client that caps or ranks tools has the same problem: Cursor (about 40
tools across all servers), Copilot, small local models.

**Status: done in v0.3.0** (0.1-0.5 and 0.7; 0.6 is an agentty change, reported
there). `help` and `run_tool` are in every profile; `tests/discover.py`
replays agentty's ranking and checks all 50 everyday tasks are reachable.
In agentty, "what can you do with Word docs?" now lists every area.

**Work.**

| Item | Done when |
|---|---|
| 0.1 A `help` tool (or `capabilities`) that lists every tool by task, one line each, with the call to make | A model that sees only `help` can name the right tool for every row of the gap list above |
| 0.2 Server instructions name the core tools and say "call help to see everything" | Instructions under 1 KB and every client shows them |
| 0.3 MCP prompts: `review_document`, `draft_document`, `fill_template`, `redline` as ready workflows | Listed by `prompts/list`, each one tested end to end |
| 0.4 MCP resources: open documents as `docx://doc1/text` and `docx://doc1/outline` | Clients that read resources see the document without a tool call |
| 0.5 Profile `essential` (about 12 tools: open, create, read, find, insert/replace/delete text, insert/delete paragraph, format, save, undo, help) for clients that rank or cap tools | A model with only `essential` finishes the e2e contract flow |
| 0.6 agentty: always ship `mcp_search_tools` when a server's tools are trimmed, and pin a server's declared core tools by default | Ask agentty "what can you do with Word docs?" and it lists every tab from this page (fix lives in the agentty repo) |
| 0.7 Tool descriptions say what they're for in a user's words ("add a row", "make this bold"), since that's what rankers match on | Ranking test: for 50 plain requests, the right tool is in the top 16 |

Test: `tests/discover.py` gives a model only what a capped client would see
and checks it finds the right tool for 50 everyday tasks.

## Phase 1: gaps in what Word documents commonly contain

Status is checked against the code.

| # | Feature | Word UI | Status | What's left |
|---|---|---|---|---|
| 1.1 | Floating pictures | Wrap Text, Position | Done (v0.3.0) | `insert_picture` takes `layout`; `format_picture`, `delete_picture`, `list_pictures` |
| 1.2 | Text boxes and shapes | Insert > Text Box, Shapes | Done | `insert_text_box`, and `insert_shape` for any DrawingML preset (arrows, stars, callouts, flowchart shapes, lines with arrowheads) with fill, outline, dash, rotation and centered text. Left: a VML fallback for very old readers |
| 1.3 | Captions and cross-references | References | Done (v0.3.0) | `insert_caption`, `insert_cross_reference`, `list_reference_targets` |
| 1.4 | Watermark | Design > Watermark | Done | `set_watermark` text (diagonal or horizontal) or picture (washout, scale), remove |
| 1.5 | Page color and borders | Design | Done | `set_page_setup` `page_color`, `page_borders` |
| 1.6 | Columns | Layout > Columns | Done | `columns`, `column_space`, `column_line`, `column_widths`; column breaks via `insert_break` |
| 1.7 | Tables | Table Layout | Done | Split cells and nested tables, `sort_table`, `convert_table`, `repeat_header`, row height. Floating tables (`format_table float`), `cant_split`, alt text, RTL tables, vertical cell text (`text_direction: up/down`) |
| 1.8 | Table of figures, index, bibliography | References | Done | `insert_toc` with `captions`, `mark_index_entry`, `insert_index`, `add_source`, `insert_citation`, `insert_bibliography` (APA, MLA, Chicago, IEEE, sources in Word's b:Sources store) |
| 1.9 | Lists: multi-level control | Define New Multilevel List | Done (picture bullets left) | `define_list` presets (numbered, legal, outline, headings, chapters, bullets, checklist) with per-level format, text, start, restart, indents, linked heading styles; `list_lists`; `apply_list num_id` |
| 1.10 | Styles: full definition | Modify Style | Done | `modify_style` sets any paragraph or run property, and for table styles whole-table borders and conditional formats (header row, total row, banded rows, first and last column). `import_styles` copies styles from a template with their based-on chains and numbering |
| 1.11 | Themes | Design > Themes | Done | `theme` reads and sets theme colors and heading/body fonts, with Word's built-in color sets; cached theme-derived colors in styles are refreshed |
| 1.12 | Charts | Insert > Chart | Done | `list_charts` reads cached data of every chart type; `insert_chart` makes column, bar, line, pie, doughnut, area and scatter charts; `set_chart_data` edits them. Left: an embedded workbook for Edit Data |
| 1.13 | Building blocks and templates | Quick Parts, .dotx | Templates done | `create_document template=` (File > New from a .dotx/.dotm/.docx, attachedTemplate set), Save As .dotx. Left: the glossary document (Quick Parts) |
| 1.14 | Mail merge and document variables | Mailings | Done | `mail_merge` from records, CSV, TSV or JSON: one file per record or one document with a section each; IF, SKIPIF, NEXT, MERGEREC. `document_variables` reads and sets `w:docVars` |
| 1.15 | Form fields and data binding | Developer | Done | Content controls, `bind_content_control` to document properties or custom XML data (both sides kept in step), `repeating_section`. Left: creating legacy form fields (read and fill work through fields) |
| 1.16 | Drop caps, text effects, ruby | Font, Paragraph | Drop caps done | `drop_cap`. Left: `w14` text effects, ruby, East Asian layout |
| 1.17 | Line numbering and hyphenation | Layout | Done | `set_page_setup` `line_numbers`, `set_settings` `auto_hyphenation`, `format_paragraph` `suppress_line_numbers` / `suppress_auto_hyphens` |
| 1.18 | Comments: modern parts | Review | Mostly done | Threads, resolved, durable ids, people. Left: @mentions |
| 1.19 | Tracked moves and formatting | Review | Moves done (v0.3.0) | Left: tracked table property changes, `rsid` stamps |
| 1.20 | Document inspection | File > Inspect Document | Done | `inspect_document` reports and removes comments, changes, personal info, hidden text, custom XML, custom properties, watermarks |

Each feature lands as tools plus engine code in `src/wml`, with:

- a unit test that writes it and validates with our validator
- a corpus file from Word that uses it (read, edit, accept/reject round-trip)
- coverage.py and fuzz.py entries
- an oxval-clean output and a LibreOffice open check

## Phase 2: seeing the page

Agents can't see layout today. Page numbers, page counts and "what does page
3 look like" only resolve when Word opens the file.

**Done:** 2.1 `render_pages` (PNG pages as MCP image content) and 2.2
`export_pdf`, through LibreOffice with pdftoppm, mutool or Ghostscript.
2.3 and 2.4: a bookmark marks every paragraph and field in a copy, LibreOffice
exports them as PDF destinations, and their pages become the page map.
Open: 2.5 and SECTIONPAGES.

| # | Item | Done when |
|---|---|---|
| 2.1 | `render_pages`: PNG of chosen pages | Rendered through LibreOffice when it's installed, with a clear error when not. Returned as MCP image content |
| 2.2 | `export_pdf` | PDF written next to the file, fonts embedded, through LibreOffice or Word on Windows/macOS when present |
| 2.3 | Page map: which paragraphs are on which page | `read_document` can say `page: 3`, from the rendered layout, cached until the next edit |
| 2.4 | Field results computed by us | PAGE, NUMPAGES, SECTIONPAGES, DATE, REF, SEQ, TOC page numbers filled in on save so files read right before Word recalculates |
| 2.5 | Visual diff | `compare_documents` can return before/after page images of the changed pages |

Test: render each corpus file, compare against images rendered by Word
(kept in `tests/corpus/rendered/`) with a perceptual diff threshold.

## Phase 3: formats

**Done:** 3.6 Markdown in (`insert_markdown`: headings, inline formatting,
links, nested lists, quotes, code, tables, rules, page breaks; tracked as one
insertion) and HTML in (`insert_html`: the same plus span styles, colspans, cell colors and embedded images). Open: the rest.

| # | Item | Notes |
|---|---|---|
| 3.1 | `.docm` and `.dotm` | Done: VBA bytes kept unchanged; Save As .docx/.dotx drops them like Word. Never run macros |
| 3.2 | `.dotx` | Done: open, edit, save as template, new document from one |
| 3.3 | Strict OOXML | Already read; add writing Strict when the input was Strict |
| 3.4 | Flat OPC (`.xml` Word documents) | Read and write the single-file form |
| 3.5 | `.doc` (Word 97-2003) | Convert in through LibreOffice when present; we don't write `.doc` |
| 3.6 | Markdown and HTML in | `insert_markdown` turns Markdown into real headings, lists, tables, links and code with Word styles; `insert_html` for pasted HTML (`w:altChunk` as a fallback Word expands on open) |
| 3.7 | Markdown out | `get_text format=markdown` covers tables, notes, comments, tracked changes |

## Phase 4: scale and speed

**Done:** 4.4 `batch` (a list of calls as one undo step, all or nothing).

| # | Item | Target |
|---|---|---|
| 4.1 | Large documents | 1,000-page document: open under 2 s, an edit under 50 ms, save under 1 s |
| 4.2 | Incremental undo | Store diffs of changed parts, not whole snapshots |
| 4.3 | Paragraph index | Ids to nodes in a map kept current, not a tree walk per call |
| 4.4 | Batch tool | `batch` runs a list of calls as one undo step and one validation |
| 4.5 | Validation cost | Validate only parts an edit touched |
| 4.6 | Benchmarks in CI | Fail on a 20% regression |

## Phase 5: proof

| # | Item | Done when |
|---|---|---|
| 5.1 | Word as an oracle | On Windows CI, Word opens every output through COM automation with no repair prompt, and its accept/reject matches ours |
| 5.2 | Bigger corpus | 2,000+ real documents (public government, academic, legal sources), each round-tripped |
| 5.3 | Long fuzz | Nightly hour-long fuzz under ASan, crashes filed automatically |
| 5.4 | Model eval | A task suite (draft, review, redline, fill a form, fix formatting) run against several models through MCP, scored on the output file. Tracks success rate per release |
| 5.5 | Spec coverage report | Which WordprocessingML elements we can read, write and edit, generated from the schema |

## Order

Phases 0, most of 1, 2.1-2.2, 3.6 and 4.4 are done. Next, by how often real
documents and agents need them:

1. ~~2.4 field results and 2.3 the page map~~ (done).
2. ~~1.9 multi-level lists and 1.10 table style conditional formats~~ (done).
3. ~~1.14 mail merge, 1.15 data binding~~ (done).
4. ~~1.7 floating tables, 1.11 themes, 1.2 shapes, 1.4 picture watermarks~~ (done).
5. ~~3.6 `insert_html`~~ (done), then Phase 4 speed work and Phase 5 proof.

## Not planned

- Running macros or VBA.
- Writing `.doc`.
- Our own layout engine. Rendering goes through LibreOffice or Word.

# Roadmap

Where docx-mcp is, what's missing, and the order we'll build it in. Every item
says what "done" means and how it gets tested. Nothing ships without passing
the Open XML SDK validator, the coverage driver and the fuzzer.

The goal: an agent can do anything in a .docx that a person can do in Word,
the file is exactly what Word itself would write, and the agent never needs
to touch raw XML to get there.

## Where we are (v0.2.1)

85 tools cover Word's Home, Insert, Layout, References and Review tabs:
documents, text, paragraphs, styles, lists, tables, pictures, links,
bookmarks, notes, fields, TOC, equations, content controls, sections,
headers and footers, comments, tracked changes, compare, protection,
properties and settings.

Every output is checked by Microsoft's Open XML SDK validator in CI: 564
corpus files, 2,256 edited/accepted/rejected outputs, all 85 tools tracked
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

Checked against the code. None of these exist today.

**Done in v0.3.0:** 1.1 floating pictures (`insert_picture` layout,
`list_pictures`, `format_picture`, `delete_picture`), 1.3 captions and
cross-references (`insert_caption`, `list_reference_targets`,
`insert_cross_reference`), 1.7 split cells and nested tables (`split_cell`;
tables in cells are addressable), 1.19 tracked moves as Word's
`moveFrom`/`moveTo`. Still open from 1.7: floating tables, sort, convert
text to/from table. Still open from 1.19: tracked table property changes,
`rsid` stamps.

| # | Feature | Word UI | What's needed |
|---|---|---|---|
| 1.1 | Floating pictures | Wrap Text, Position | `wp:anchor` with wrap square/tight/behind/in front, position relative to page, margin or paragraph. `insert_picture` gains `wrap` and `position`; new `move_picture`, `resize_picture`, `delete_picture`, `list_pictures` |
| 1.2 | Text boxes and shapes | Insert > Text Box, Shapes | `wps:wsp` with `txbxContent`, plus VML fallback in `mc:AlternateContent` like Word writes. Text inside is addressable as a story (`part#id` already works for headers) |
| 1.3 | Captions and cross-references | References > Insert Caption, Cross-reference | `insert_caption` (Figure/Table/Equation with `SEQ`; `update_fields` already renumbers them), `insert_cross_reference` to a heading, bookmark, caption or note, with `REF`/`PAGEREF`/`NOTEREF` and the hidden `_Ref` bookmark Word adds |
| 1.4 | Watermark | Design > Watermark | Text or picture watermark in every header, as Word draws it (VML shape in the header with `PowerPlusWaterMarkObject`) |
| 1.5 | Page color and borders | Design > Page Color, Page Borders | `w:background`, `w:displayBackgroundShape`, `w:pgBorders` through `set_page_setup` |
| 1.6 | Columns per section with breaks | Layout > Columns | Already sets `w:cols`; add unequal widths, a line between, and column breaks that respect sections |
| 1.7 | Table: floating, nested, cell split, header repeat | Table Layout | `w:tblpPr` positioning, `split_cell` (the reverse of merge), insert a table in a cell, vertical cell text, row height rules, `set_cell` borders per side, sort rows, table from/to text |
| 1.8 | Table of figures, index, bibliography | References | `insert_table_of_figures` (`TOC \c "Figure"`), `mark_index_entry` (`XE`) + `insert_index` (`INDEX`), citations (`CITATION`) with the `customXml` sources part, `insert_bibliography` |
| 1.9 | Lists: real multi-level control | Define New Multilevel List | Edit a list's levels (format, text, indent, start, restart after), picture bullets (`w:numPicBullet`), link levels to heading styles, legal numbering |
| 1.10 | Styles: full definition | Modify Style > Format | Borders, shading, tabs, numbering, table style conditional formats (header row, banded rows), linked styles, style separators; `import_styles` from another document or template |
| 1.11 | Themes | Design > Themes, Colors, Fonts | Read and set theme colors and fonts; resolve `themeColor`/`asciiTheme` in effective formatting |
| 1.12 | Charts | Insert > Chart | Read the cached chart data and type; insert a basic bar/line/pie chart with its embedded workbook so Word can edit it. Start read-only |
| 1.13 | Building blocks and templates | Quick Parts, .dotx | Read the glossary document, insert a building block, create a document from a `.dotx`, save as `.dotx` |
| 1.14 | Mail merge and document variables | Mailings | Fill `MERGEFIELD`s from JSON rows (one document or one per row), `w:docVars`, `DOCVARIABLE` |
| 1.15 | Form fields and data binding | Developer | Legacy form fields (`FORMTEXT`, `FORMCHECKBOX`), content controls bound to `customXml` (`w:dataBinding`), repeating sections, fill a form from JSON |
| 1.16 | Drop caps, text effects, ruby, East Asian layout | Font dialog, Paragraph | `w:framePr` drop cap, `w14` text effects, `w:ruby`, `w:eastAsianLayout`, `w:fitText` |
| 1.17 | Line numbering and hyphenation | Layout | Already writes the elements; expose per section and per paragraph with suppress options |
| 1.18 | Comments: modern parts complete | Review | Mentions (`w15:people`, `@name`), comment dates in `commentsExtensible`, comments on comments in headers and notes |
| 1.19 | Tracked formatting and moves, Word-exact | Review | Tracked moves as `w:moveFrom`/`w:moveTo` with move ranges (today a tracked move is delete + insert), track table property changes, `rsid` stamps so Word's compare merges cleanly |
| 1.20 | Document inspection and cleanup | File > Inspect Document | Remove personal info, hidden text, comments, revisions, custom XML, document properties in one call |

Each feature lands as tools plus engine code in `src/wml`, with:

- a unit test that writes it and validates with our validator
- a corpus file from Word that uses it (read, edit, accept/reject round-trip)
- coverage.py and fuzz.py entries
- an oxval-clean output and a LibreOffice open check

## Phase 2: seeing the page

Agents can't see layout today. Page numbers, page counts and "what does page
3 look like" only resolve when Word opens the file.

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

| # | Item | Notes |
|---|---|---|
| 3.1 | `.docm` and `.dotm` | Open, edit and save while keeping the VBA project bytes unchanged. Never run macros |
| 3.2 | `.dotx` | Template in, document out (also 1.13) |
| 3.3 | Strict OOXML | Already read; add writing Strict when the input was Strict |
| 3.4 | Flat OPC (`.xml` Word documents) | Read and write the single-file form |
| 3.5 | `.doc` (Word 97-2003) | Convert in through LibreOffice when present; we don't write `.doc` |
| 3.6 | Markdown and HTML in | `insert_markdown` turns Markdown into real headings, lists, tables, links and code with Word styles; `insert_html` for pasted HTML (`w:altChunk` as a fallback Word expands on open) |
| 3.7 | Markdown out | `get_text format=markdown` covers tables, notes, comments, tracked changes |

## Phase 4: scale and speed

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

1. Phase 0 first. It fixes most of what users hit today without new engine
   code, and every later feature depends on being discoverable.
2. Phase 1 in this order: 1.3 captions and cross-references, 1.1 floating
   pictures, 1.7 tables, 1.9 lists, 1.19 tracked moves, 1.10 styles, then
   the rest. That's roughly how often real documents use them.
3. Phase 2.1 and 2.2 alongside Phase 1: rendering is the biggest thing agents
   can't do at all.
4. Phases 3 to 5 after.

## Not planned

- Running macros or VBA.
- Writing `.doc`.
- Our own layout engine. Rendering goes through LibreOffice or Word.

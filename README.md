# docx-mcp

An MCP server that edits Word documents the way Word does. Built in C++23 on
[mcp-cpp](https://github.com/1ay1/mcp-cpp). Nothing to install at build or
run time: every library is vendored or a submodule. `-DDOCX_STATIC=ON` gives
one fully static binary (about 9 MB in Release).

## What it does

85 tools cover what you do in Word's ribbon:

- **Text**: read, find, insert, replace, delete, find and replace (with regex), split and join paragraphs, move paragraphs, undo.
- **Formatting**: every Font and Paragraph setting, clear formatting.
- **Styles**: Word's built-in styles (added on first use with Word's own definitions), custom styles, and style resolution.
- **Lists**: bullets and numbering, levels, restarting the numbering.
- **Review**: Track Changes for every edit, accept and reject (all, by id, by author), comments with replies, resolve and delete, and Compare.
- **Tables**: insert, rows and columns, merge, cell and table formatting, and Word's table styles.
- **Insert**: pictures, breaks, section breaks, hyperlinks, bookmarks, footnotes and endnotes, fields, table of contents, equations, content controls.
- **Layout**: page size, orientation, margins, columns, headers and footers with page numbers.
- **File**: properties, settings, Restrict Editing with Word's password hash, validation, raw XML.

## Correctness

Every file it writes is checked by three independent consumers:

1. Its own validator. It is generated from the Open XML SDK's schema data and covers 4315 element types, including the Office 2010–365 extensions and the SDK's semantic rules. On the 563-file OXPT corpus it reports exactly what Microsoft's validator reports (83/83).
2. Microsoft's Open XML SDK validator (`tools/oxval`), targeting Microsoft 365.
3. LibreOffice, which opens each file, counts revisions and comments, and renders it to PDF (`tools/lo_check.py`).

Behaviour is checked against Word's own output:

- Accept and reject match Word-produced expected files on 104 cases (`tests/corpus/oxpt/RP`).
- Compare reproduces 29 of 29 WmlComparer pairs exactly.
- The protection hash verifies a password set by Word (`tests/corpus/poi/bug56076.docx`).
- A battery of edits on all 563 corpus files adds no schema issues.

## Build

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # add -DDOCX_STATIC=ON for a static binary
cmake --build build
```

## Use

```sh
build/docx-mcp --author "Your Name"
```

Example MCP client config:

```json
{ "mcpServers": { "docx": { "command": "/path/to/docx-mcp", "args": ["--author", "Your Name"] } } }
```

The usual flow: `open_document`, then `read_document` (which gives stable paragraph ids), then edits, then `save_document`. Turn on Track Changes with `set_track_changes`, or pass `track: true` on any edit.

## Tests

```sh
cd build/tests
./edit_test && ./comments_test && ./structure_test && ./docops_test
./revisions_corpus ../../tests/corpus/oxpt/RP
./robustness ../../tests/corpus/oxpt
python3 ../../tests/e2e.py ../docx-mcp /tmp/e2e
```

The external oracles are optional. Build oxval with `dotnet build -c Release -o /tmp/oxval-bin` in `tools/oxval`. LibreOffice checks need `soffice` and python3-uno.

## Limits

Page numbers in a TOC and `PAGE`/`NUMPAGES` results need page layout. The
server writes correct fields and sets Word to refresh them on open. It does
not paginate by itself.

## Layout

- `src/opc`: zip package, content types, relationships
- `src/schema`: generated schema tables, ordering, validation
- `src/xml`: namespace normalization, Markup Compatibility
- `src/wml`: the Word engine (text, edits, revisions, comments, styles, tables, fields, ...)
- `src/mcp`: sessions, the tool catalog, and the stdio server
- `assets/word`: Word's blank document parts and built-in style definitions
- `tools`: schema generator, style harvester, and oracle scripts

Third-party: pugixml and miniz (vendored), plus nlohmann/json, jsonrpc-cpp and mcp-cpp (submodules). Test corpus from Open-Xml-PowerTools (MIT) and Apache POI. Schema data from the Open XML SDK (MIT).

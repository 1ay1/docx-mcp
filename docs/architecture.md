# Architecture

```
src/
  opc/      zip package: parts, [Content_Types].xml, relationships
  xml/      namespace normalization, Markup Compatibility, DOM helpers
  schema/   generated schema tables, child ordering, validation
  util/     hashes, base64, ids, UTF-8, embedded assets
  wml/      the Word engine
    document    parts on demand, ids, authors, blank document
    text        visible-text model, run splitting
    edit        insert / delete / replace / format, find and replace
    props       Word property names <-> XML
    revisions   accept / reject for every revision type
    comments    modern comments
    styles      built-ins, custom styles, effective formatting
    tables      tables
    content     pictures, sections, headers and footers, notes, bookmarks, links
    fields      fields, TOC, content controls, lists, equations
    docops      protection, properties, settings, statistics, reading, compare
  mcp/      sessions, the tool catalog, the stdio server
```

## Ideas that hold it together

**One visible-text model.** `wml::analyze(p)` turns a paragraph into atoms:
characters, tabs, breaks and objects. Each atom points back at its run, its
XML node and its revision state. Every edit is expressed in visible offsets.
`split_at` and `isolate` cut runs (and revision containers) at exact
offsets, so an edit only ever touches whole runs.

**Schema-ordered writes.** Child elements are never appended blindly.
`schema::ensure_child` / `insert_child` put each one at its position in the
content model. That's how a `w:b` lands in the right place among 40 possible
`w:rPr` children.

**Canonical prefixes.** On load, every part's namespaces are rewritten to the
canonical prefixes (`w`, `w14`, `r`, ...) and Strict namespaces are mapped to
Transitional. The rest of the code can then say `"w:p"`. On save, declarations
and `mc:Ignorable` are rebuilt from what is actually used.

**Revisions as a property of every edit.** Each editing function takes an
`EditOpts` and produces either a direct change or Word's tracked markup for
the same change. The accept/reject engine is the inverse, and the tests
check the round trip: accept gives the intended text, reject gives the
original.

**Ids.** Annotation ids are drawn from one counter above every existing id.
Ids that copying could duplicate are renumbered at save. Paragraph ids
(`w14:paraId`) are random, below `0x80000000`, unique in the package, and
given to every paragraph the agent might address.

**Sessions.** Documents stay in memory between calls. Each mutating call
snapshots the package first, which is what makes `undo` cheap and exact.

## Data generation

- `tools/gen_schema.py` -> `src/schema/schema_data.cpp` (committed). Set
  `OXSDK_GENERATED` to the SDK's generated sources to refresh the semantic
  constraints.
- `tools/harvest_word.py` -> `assets/word/builtin_styles.xml`,
  `latent_styles.xml`.
- `cmake/embed.cmake` compiles `assets/**/*.xml` into the binary.
- `tools/gen_tool_docs.py` -> `docs/tools.md`.

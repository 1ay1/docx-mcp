#!/usr/bin/env python3
"""Print the e2e drafting calls as JSON-RPC lines (for gdb/valgrind replays)."""
import json
def m(i, meth, p): print(json.dumps({"jsonrpc": "2.0", "id": i, "method": meth, "params": p}))
m(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "x", "version": "1"}})
c = lambda i, n, **a: m(i, "tools/call", {"name": n, "arguments": a})
c(2, "create_document", path="/tmp/replay.docx", author="A", title="Services Agreement")
c(3, "insert_text", doc="doc1", paragraph="1", text="Services Agreement")
c(4, "apply_style", doc="doc1", paragraphs=["1"], style="Title")
c(5, "insert_paragraph", doc="doc1", paragraph="1", text="1. Scope\nThe Provider will deliver.\n2. Fees\nThe Client pays 1,000 USD per month within 30 days.")
c(6, "apply_style", doc="doc1", paragraphs=["2", "4"], style="Heading 1")
c(7, "format_text", doc="doc1", paragraph="5", match="1,000 USD", format={"bold": True})
c(8, "insert_table", doc="doc1", paragraph="5", rows=[["Item", "Price"], ["Setup", "500"]], style="Grid Table 4 Accent 1")
c(9, "set_header_footer", doc="doc1", kind="footer", text="Page {PAGE} of {NUMPAGES}", alignment="center")
c(10, "insert_toc", doc="doc1", paragraph="2", title="Contents")
c(11, "set_properties", doc="doc1", core={"subject": "MSA"}, custom={"Client": "Acme Corp"})
c(12, "save_document", doc="doc1")

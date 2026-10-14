// Debug probe: apply accept/reject to one file and dump the body XML.
#include <cstdio>
#include <cstring>

#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "xml/xml.hpp"

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: probe_rev <file> accept|reject|none"); return 2; }
    auto d = docx::wml::Document::open(argv[1]);
    docx::wml::RevFilter f;
    if (!std::strcmp(argv[2], "accept")) docx::wml::accept_revisions(d, f);
    else if (!std::strcmp(argv[2], "reject")) docx::wml::reject_revisions(d, f);
    std::puts(docx::xml::to_string(d.body(), false).c_str());
}

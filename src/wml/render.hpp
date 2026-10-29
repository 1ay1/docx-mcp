// Seeing the page: PDF export and page images, through LibreOffice when it's
// installed (we don't lay pages out ourselves). Everything runs on a saved
// copy in a temp folder; the document isn't touched.
#pragma once
#include <map>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "util/util.hpp"

namespace docx::wml {

class Document;

// Path of the renderer, or "" when none is installed.
std::string renderer();

// The document as PDF bytes. Throws with install advice when there's no renderer.
util::Bytes export_pdf(Document& d, int timeout_ms = 120000);

struct PageImage { int page; util::Bytes png; };
// PNG images of pages [first, last] (1-based; last 0 = first). dpi 30-300.
// pages_total is set to the document's page count.
std::vector<PageImage> render_pages(Document& d, int first, int last, int dpi, int& pages_total,
                                    int timeout_ms = 120000);

// Where things land on the laid-out pages (physical page numbers, 1-based).
struct PageMap {
    int total = 0;
    std::vector<int> paras;               // nth paragraphs(body, false) -> page it starts on (0 = unknown)
    std::map<std::string, int> para;      // w14:paraId -> page
    std::map<std::string, int> bookmark;  // bookmark name -> page
    std::vector<int> fields;              // nth field_anchors(body) entry -> page (0 = unknown)
};
// Lays the document out through LibreOffice. Throws when there's no renderer.
PageMap page_map(Document& d, int timeout_ms = 120000);

// Field starts in the body, in document order: w:fldChar begin and w:fldSimple.
std::vector<pugi::xml_node> field_anchors(pugi::xml_node body);

}  // namespace docx::wml

// Seeing the page: PDF export and page images, through LibreOffice when it's
// installed (we don't lay pages out ourselves). Everything runs on a saved
// copy in a temp folder; the document isn't touched.
#pragma once
#include <string>
#include <vector>

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

}  // namespace docx::wml

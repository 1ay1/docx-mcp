#include "wml/render.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <regex>
#include <stdexcept>

#include "util/fsio.hpp"
#include "util/process.hpp"
#include "wml/document.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> office_candidates() {
    std::vector<std::string> v = {"soffice", "libreoffice"};
#if defined(_WIN32)
    for (auto base : {"C:/Program Files/LibreOffice/program/soffice.exe",
                      "C:/Program Files (x86)/LibreOffice/program/soffice.exe"})
        v.push_back(base);
#elif defined(__APPLE__)
    v.push_back("/Applications/LibreOffice.app/Contents/MacOS/soffice");
#endif
    if (auto e = util::env("DOCX_MCP_SOFFICE"); !e.empty()) v.insert(v.begin(), e);
    return v;
}

// A temp folder removed when it goes out of scope.
struct TempDir {
    fs::path p;
    TempDir() {
        std::error_code ec;
        p = fs::temp_directory_path(ec) / util::u8path("docx-mcp-" + util::hex(util::random_bytes(8)));
        fs::create_directories(p, ec);
        if (ec) throw std::runtime_error("cannot create a temp folder: " + ec.message());
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
};

// Convert the document to PDF in `dir`, return the PDF path.
fs::path to_pdf(Document& d, const fs::path& dir, int timeout_ms) {
    std::string office = renderer();
    if (office.empty())
        throw std::runtime_error("rendering needs LibreOffice, which isn't installed (or set DOCX_MCP_SOFFICE to "
                                 "soffice's path). Install: https://www.libreoffice.org/download/");
    auto in = dir / "doc.docx";
    util::write_file_atomic(util::path_utf8(in), d.to_bytes());
    // a private profile: works alongside a running LibreOffice, and leaves the user's alone
    auto profile = dir / "profile";
    std::string prof_url = "file:///" + util::path_utf8(fs::absolute(profile));
    std::replace(prof_url.begin(), prof_url.end(), '\\', '/');
    if (prof_url.rfind("file:////", 0) == 0) prof_url.erase(7, 1);
    auto r = util::run_process({office, "--headless", "--norestore", "--nologo", "-env:UserInstallation=" + prof_url,
                                "--convert-to", "pdf", "--outdir", util::path_utf8(dir), util::path_utf8(in)},
                               timeout_ms);
    auto out = dir / "doc.pdf";
    std::error_code ec;
    if (r.timed_out) throw std::runtime_error("rendering timed out (the document may be very large)");
    if (!fs::exists(out, ec))
        throw std::runtime_error("LibreOffice couldn't convert the document (exit " + std::to_string(r.exit_code) +
                                 "): " + util::trim(r.output.substr(0, 300)));
    return out;
}

int pdf_pages(const util::Bytes& pdf) {
    // count page objects: "/Type /Page" not followed by "s"
    std::string s(pdf.begin(), pdf.end());
    int n = 0;
    for (std::size_t i = 0; (i = s.find("/Type", i)) != std::string::npos; i += 5) {
        std::size_t j = i + 5;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\r' || s[j] == '\n')) j++;
        if (s.compare(j, 5, "/Page") == 0 && (j + 5 >= s.size() || s[j + 5] != 's')) n++;
    }
    return n;
}

}  // namespace

std::string renderer() { return util::find_program(office_candidates()); }

util::Bytes export_pdf(Document& d, int timeout_ms) {
    TempDir t;
    return util::read_file(util::path_utf8(to_pdf(d, t.p, timeout_ms)));
}

std::vector<PageImage> render_pages(Document& d, int first, int last, int dpi, int& total, int timeout_ms) {
    if (dpi < 30 || dpi > 300) throw std::runtime_error("dpi must be 30 to 300");
    TempDir t;
    auto pdf = to_pdf(d, t.p, timeout_ms);
    auto bytes = util::read_file(util::path_utf8(pdf));
    total = pdf_pages(bytes);
    if (first < 1) first = 1;
    if (last < first) last = first;
    if (total > 0 && first > total)
        throw std::runtime_error("page " + std::to_string(first) + " doesn't exist (the document has " +
                                 std::to_string(total) + ")");
    if (total > 0) last = std::min(last, total);
    if (last - first >= 20) throw std::runtime_error("render at most 20 pages at a time");
    // rasterize with whichever PDF tool is installed
    std::string prefix = util::path_utf8(t.p / "page");
    std::string f = std::to_string(first), l = std::to_string(last), r = std::to_string(dpi);
    util::ProcResult pr;
    if (auto p = util::find_program({"pdftoppm"}); !p.empty())
        pr = util::run_process({p, "-png", "-r", r, "-f", f, "-l", l, util::path_utf8(pdf), prefix}, timeout_ms);
    else if (auto m = util::find_program({"mutool"}); !m.empty())
        pr = util::run_process({m, "draw", "-r", r, "-o", prefix + "-%d.png", util::path_utf8(pdf), f + "-" + l}, timeout_ms);
    else if (auto g = util::find_program({"gs", "gswin64c", "gswin32c"}); !g.empty())
        pr = util::run_process({g, "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=png16m", "-r" + r,
                                "-dFirstPage=" + f, "-dLastPage=" + l, "-sOutputFile=" + prefix + "-%d.png",
                                util::path_utf8(pdf)},
                               timeout_ms);
    else
        throw std::runtime_error("page images need a PDF rasterizer: install poppler (pdftoppm), mupdf (mutool) or "
                                 "Ghostscript. export_pdf works without one.");
    // collect page-N.png / page-0N.png, in page order
    std::vector<PageImage> out;
    std::regex num(R"(page-0*(\d+)\.png)");
    std::error_code ec;
    for (auto& e : fs::directory_iterator(t.p, ec)) {
        std::smatch m;
        std::string name = util::path_utf8(e.path().filename());
        if (!std::regex_match(name, m, num)) continue;
        int n = std::stoi(m[1]);
        // Ghostscript and mutool number from 1 relative to the range
        if (n < first) n = first + n - 1;
        out.push_back({n, util::read_file(util::path_utf8(e.path()))});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.page < b.page; });
    if (out.empty())
        throw std::runtime_error("rasterizing failed: " + util::trim(pr.output.substr(0, 300)));
    return out;
}

std::vector<pugi::xml_node> field_anchors(pugi::xml_node body) {
    std::vector<pugi::xml_node> v;
    std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
        for (auto c : n.children()) {
            if (c.type() != pugi::node_element) continue;
            std::string_view q = c.name();
            if (q == "mc:Fallback") continue;
            if (q == "w:fldSimple") v.push_back(c);
            else if (q == "w:fldChar" && xml::attr(c, "w:fldCharType") == "begin") v.push_back(c);
            walk(c);
        }
    };
    walk(body);
    return v;
}

namespace {

// PDF name objects escape bytes as #XX; LibreOffice also writes "_" as "5F"
// at the start of a destination name. Our markers avoid both by using
// letters and digits only.
constexpr const char* kMark = "zzpm";

// Page object number -> page index, from the /Kids arrays of the page tree,
// and destination name -> page object, from the /Dests dictionary.
struct PdfDests {
    std::map<std::string, int> page_of;  // name -> 1-based page
    int total = 0;
};

PdfDests read_dests(const util::Bytes& pdf) {
    std::string s(pdf.begin(), pdf.end());
    PdfDests r;
    // page objects in order: walk "/Type/Page" objects by position isn't the
    // page order; read /Kids of the root /Pages instead (LibreOffice writes a flat tree)
    std::vector<int> order;
    std::regex kids(R"(/Type\s*/Pages[^>]*?/Kids\s*\[([^\]]*)\])");
    std::regex ref(R"((\d+)\s+0\s+R)");
    std::smatch m;
    if (std::regex_search(s, m, kids)) {
        std::string k = m[1];
        for (std::sregex_iterator it(k.begin(), k.end(), ref), e; it != e; ++it) order.push_back(std::stoi((*it)[1]));
    }
    r.total = (int)order.size();
    std::map<int, int> idx;
    for (std::size_t i = 0; i < order.size(); i++) idx[order[i]] = (int)i + 1;
    std::regex dest(std::string("/") + kMark + R"(([A-Za-z0-9]+)\s*\[\s*(\d+)\s+0\s+R)");
    for (std::sregex_iterator it(s.begin(), s.end(), dest), e; it != e; ++it) {
        int obj = std::stoi((*it)[2]);
        if (auto f = idx.find(obj); f != idx.end()) r.page_of[(*it)[1]] = f->second;
    }
    return r;
}

// A zero-width bookmark right before `n` (a paragraph child) or at the start of paragraph p.
void mark_before(pugi::xml_node n, const std::string& name, int id) {
    auto bs = n.parent().insert_child_before("w:bookmarkStart", n);
    xml::set_attr(bs, "w:id", std::to_string(id));
    xml::set_attr(bs, "w:name", name);
    auto be = n.parent().insert_child_before("w:bookmarkEnd", n);
    xml::set_attr(be, "w:id", std::to_string(id));
}

void mark_para(pugi::xml_node p, const std::string& name, int id) {
    auto bs = p.child("w:pPr") ? p.insert_child_after("w:bookmarkStart", p.child("w:pPr")) : p.prepend_child("w:bookmarkStart");
    xml::set_attr(bs, "w:id", std::to_string(id));
    xml::set_attr(bs, "w:name", name);
    auto be = p.insert_child_after("w:bookmarkEnd", bs);
    xml::set_attr(be, "w:id", std::to_string(id));
}

}  // namespace

PageMap page_map(Document& d, int timeout_ms) {
    std::string office = renderer();
    if (office.empty())
        throw std::runtime_error("page numbers need LibreOffice to lay the pages out, and it isn't installed (or set "
                                 "DOCX_MCP_SOFFICE to soffice's path)");
    // work on a copy: markers are bookmarks that become PDF destinations
    Document c = Document::from_bytes(d.snapshot());
    auto body = c.body();
    auto ps = paragraphs(body, false);
    auto fs = field_anchors(body);
    int id = 900000000;
    for (std::size_t i = 0; i < ps.size(); i++) mark_para(ps[i], kMark + std::string("p") + std::to_string(i), id++);
    for (std::size_t i = 0; i < fs.size(); i++) {
        // a fldChar sits inside a w:r: mark before the run
        auto at = std::string_view(fs[i].name()) == "w:fldSimple" ? fs[i] : fs[i].parent();
        if (std::string_view(at.parent().name()) == "w:p" || std::string_view(at.parent().name()) == "w:hyperlink" ||
            std::string_view(at.parent().name()) == "w:ins" || std::string_view(at.parent().name()) == "w:sdtContent")
            mark_before(at, kMark + std::string("f") + std::to_string(i), id++);
    }
    // user bookmarks: a marker paragraph-level start next to each named start
    std::vector<std::string> names;
    for (auto b : xml::descendants(body, "w:bookmarkStart")) {
        std::string n = xml::attr(b, "w:name");
        if (n.empty() || util::starts_with(n, kMark)) continue;
        names.push_back(n);
        mark_before(b, kMark + std::string("b") + std::to_string(names.size() - 1), id++);
    }
    TempDir t;
    auto in = t.p / "doc.docx";
    util::write_file_atomic(util::path_utf8(in), c.to_bytes());
    auto profile = t.p / "profile";
    std::string prof_url = "file:///" + util::path_utf8(fs::absolute(profile));
    std::replace(prof_url.begin(), prof_url.end(), '\\', '/');
    if (prof_url.rfind("file:////", 0) == 0) prof_url.erase(7, 1);
    auto r = util::run_process({office, "--headless", "--norestore", "--nologo", "-env:UserInstallation=" + prof_url,
                                "--convert-to",
                                "pdf:writer_pdf_Export:{\"ExportBookmarksToPDFDestination\":{\"type\":\"boolean\",\"value\":\"true\"}}",
                                "--outdir", util::path_utf8(t.p), util::path_utf8(in)},
                               timeout_ms);
    auto out = t.p / "doc.pdf";
    std::error_code ec;
    if (r.timed_out) throw std::runtime_error("laying out the pages timed out (the document may be very large)");
    if (!fs::exists(out, ec))
        throw std::runtime_error("LibreOffice couldn't lay the document out (exit " + std::to_string(r.exit_code) + ")");
    auto pdf = util::read_file(util::path_utf8(out));
    auto dests = read_dests(pdf);
    PageMap pm;
    pm.total = dests.total ? dests.total : pdf_pages(pdf);
    auto get = [&](const std::string& k) {
        auto it = dests.page_of.find(k);
        return it == dests.page_of.end() ? 0 : it->second;
    };
    // paragraphs with no destination (hidden, empty in a way LO drops) take the previous page
    int last = 1;
    pm.paras.resize(ps.size());
    auto orig = paragraphs(d.body(), false);
    for (std::size_t i = 0; i < ps.size(); i++) {
        int pg = get("p" + std::to_string(i));
        if (!pg) pg = last;
        last = pg;
        pm.paras[i] = pg;
        if (i < orig.size()) {
            auto pid = para_id(orig[i]);
            if (!pid.empty()) pm.para[pid] = pg;
        }
    }
    pm.fields.resize(fs.size());
    for (std::size_t i = 0; i < fs.size(); i++) {
        int pg = get("f" + std::to_string(i));
        if (!pg) {
            // fall back to the paragraph's page
            auto p = xml::ancestor(fs[i], "w:p");
            for (std::size_t k = 0; k < ps.size(); k++)
                if (ps[k] == p) { pg = pm.paras[k]; break; }
        }
        pm.fields[i] = pg;
    }
    for (std::size_t i = 0; i < names.size(); i++)
        if (int pg = get("b" + std::to_string(i))) pm.bookmark[names[i]] = pg;
    return pm;
}

}  // namespace docx::wml

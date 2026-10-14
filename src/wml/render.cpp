#include "wml/render.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <stdexcept>

#include "util/fsio.hpp"
#include "util/process.hpp"
#include "wml/document.hpp"

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

}  // namespace docx::wml

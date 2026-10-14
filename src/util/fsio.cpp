#include "util/fsio.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace docx::util {

namespace fs = std::filesystem;

fs::path u8path(std::string_view s) {
#if defined(_WIN32)
    std::u16string w = to_u16(s);
    return fs::path(std::wstring(w.begin(), w.end()));
#else
    return fs::path(std::string(s));
#endif
}

std::string path_utf8(const fs::path& p) {
#if defined(_WIN32)
    std::wstring w = p.wstring();
    std::u32string u;
    for (std::size_t i = 0; i < w.size(); i++) {
        char32_t c = char32_t(w[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
            char32_t lo = char32_t(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        u.push_back(c);
    }
    return to_utf8(u);
#else
    return p.string();
#endif
}

std::string env(const char* name) {
#if defined(_WIN32)
    std::u16string wn = to_u16(name);
    std::wstring key(wn.begin(), wn.end());
    DWORD n = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!n) return "";
    std::wstring buf(n, L'\0');
    n = GetEnvironmentVariableW(key.c_str(), buf.data(), n);
    buf.resize(n);
    return path_utf8(fs::path(buf));
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

std::string normalize_path(std::string_view in) {
    std::string s(trim(in));
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);  // models often quote paths
    if (s.empty()) throw std::runtime_error("empty path");
    if (s == "~" || s.rfind("~/", 0) == 0 || s.rfind("~\\", 0) == 0) {
        std::string home = env("HOME");
#if defined(_WIN32)
        if (home.empty()) home = env("USERPROFILE");
#endif
        if (!home.empty()) s = home + s.substr(1);
    }
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
#if defined(_WIN32)
        if (s.size() > 2 && s[0] == '/' && s[2] == ':') s = s.substr(1);  // file:///C:/x
#endif
    }
    std::error_code ec;
    fs::path p = fs::absolute(u8path(s), ec);
    if (ec) p = u8path(s);
    return path_utf8(p.lexically_normal());
}

Bytes read_file(std::string_view path) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + std::string(path));
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool file_exists(std::string_view path) {
    std::error_code ec;
    return fs::is_regular_file(u8path(path), ec);
}

std::uintmax_t file_size(std::string_view path) {
    std::error_code ec;
    auto n = fs::file_size(u8path(path), ec);
    return ec ? 0 : n;
}

void write_file_atomic(std::string_view path, const Bytes& data) {
    fs::path target = u8path(path);
    std::error_code ec;
    // saving to a symlink updates the file it points at, like Word
    if (fs::is_symlink(target, ec)) {
        auto real = fs::canonical(target, ec);
        if (!ec) target = real;
    }
    fs::path dir = target.parent_path();
    if (!dir.empty() && !fs::exists(dir, ec)) throw std::runtime_error("folder does not exist: " + path_utf8(dir));
    fs::path tmp = target;
    tmp += u8path(".tmp~" + std::to_string(random_u32()));
#if defined(_WIN32)
    {
        FILE* f = _wfopen(tmp.c_str(), L"wbc");  // 'c': fflush commits to disk
        if (!f) throw std::runtime_error("cannot write " + std::string(path) + " (permission denied or read-only location)");
        bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = ok && std::fflush(f) == 0 && _commit(_fileno(f)) == 0;
        std::fclose(f);
        if (!ok) {
            fs::remove(tmp, ec);
            throw std::runtime_error("write failed for " + std::string(path) + " (disk full?)");
        }
    }
#else
    {
        // keep the old file's permissions when replacing it
        mode_t mode = 0666;
        struct stat st{};
        if (::stat(target.c_str(), &st) == 0) mode = st.st_mode & 07777;
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
        if (fd < 0) throw std::runtime_error("cannot write " + std::string(path) + " (permission denied or read-only location)");
        if (::stat(target.c_str(), &st) == 0) (void)::fchmod(fd, mode);
        const std::uint8_t* p = data.data();
        std::size_t left = data.size();
        bool ok = true;
        while (left) {
            ssize_t w = ::write(fd, p, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            p += w;
            left -= std::size_t(w);
        }
        // the data must be on disk before the rename makes it the file
        if (ok && ::fsync(fd) != 0) ok = false;
        ::close(fd);
        if (!ok) {
            fs::remove(tmp, ec);
            throw std::runtime_error("write failed for " + std::string(path) + " (disk full?)");
        }
    }
#endif
    // fs::rename replaces an existing file on every platform (MoveFileEx on
    // Windows). Word holds files it has open; retry for a moment, then say so.
    for (int attempt = 0;; attempt++) {
        fs::rename(tmp, target, ec);
        if (!ec) {
#if !defined(_WIN32)
            // and the rename itself must survive a crash
            int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
#endif
            return;
        }
        if (attempt >= 10) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fs::remove(tmp, ec);
    throw std::runtime_error("cannot replace " + std::string(path) +
                             " - it may be open in Word or another program. Close it or save to a different path.");
}

void set_binary_stdio() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

}  // namespace docx::util

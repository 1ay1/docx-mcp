#include "util/process.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "util/fsio.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace docx::util {

namespace fs = std::filesystem;

std::string find_program(const std::vector<std::string>& names) {
    std::string path = env("PATH");
#if defined(_WIN32)
    const char sep = ';';
    const std::vector<std::string> exts = {".exe", ".com", ""};
#else
    const char sep = ':';
    const std::vector<std::string> exts = {""};
#endif
    std::error_code ec;
    for (auto& n : names) {
        if (n.find('/') != std::string::npos || n.find('\\') != std::string::npos) {
            if (fs::is_regular_file(u8path(n), ec)) return n;
            continue;
        }
        for (auto& dir : split(path, sep)) {
            if (dir.empty()) continue;
            for (auto& e : exts) {
                auto p = u8path(dir) / u8path(n + e);
                if (fs::is_regular_file(p, ec)) return path_utf8(p);
            }
        }
    }
    return "";
}

#if defined(_WIN32)

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
// CommandLineToArgvW quoting rules.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    int bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += c; bs = 0; continue; }
        o.append(bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}
}  // namespace

ProcResult run_process(const std::vector<std::string>& argv, int timeout_ms) {
    ProcResult r;
    if (argv.empty()) return r;
    std::string exe = find_program({argv[0]});
    if (exe.empty()) return r;
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); i++) cmd += (i ? L" " : L"") + quote(widen(i == 0 ? exe : argv[i]));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring wexe = widen(exe);
    BOOL ok = CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return r; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) break;
            if (r.output.size() < 65536) r.output.append(buf, got);
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            r.timed_out = true;
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = r.timed_out ? -1 : (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return r;
}

#else

ProcResult run_process(const std::vector<std::string>& argv, int timeout_ms) {
    ProcResult r;
    if (argv.empty()) return r;
    std::string exe = find_program({argv[0]});
    if (exe.empty()) return r;
    int pipefd[2];
    if (pipe(pipefd) != 0) return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    // its own process group, so a timeout kills helpers it started too
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args[0] = const_cast<char*>(exe.c_str());
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe.c_str(), &fa, &at, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(pipefd[1]);
    if (rc != 0) { close(pipefd[0]); return r; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    bool open = true;
    int status = 0;
    for (;;) {
        if (open) {
            pollfd pf{pipefd[0], POLLIN, 0};
            if (poll(&pf, 1, 50) > 0) {
                ssize_t n = read(pipefd[0], buf, sizeof buf);
                if (n > 0) { if (r.output.size() < 65536) r.output.append(buf, (std::size_t)n); }
                else open = false;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            r.timed_out = true;
            break;
        }
    }
    // drain what's left
    while (open) {
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n <= 0) break;
        if (r.output.size() < 65536) r.output.append(buf, (std::size_t)n);
    }
    close(pipefd[0]);
    if (!r.timed_out) r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return r;
}

#endif

}  // namespace docx::util

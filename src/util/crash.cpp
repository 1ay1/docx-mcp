#include "util/crash.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#include <unistd.h>
#define DOCX_HAVE_EXECINFO 1
#endif

namespace docx::util {

namespace {

#if defined(_WIN32)
LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "docx-mcp: crashed with exception 0x%08lX at %p. Please report this with the input that caused it.\n",
                 info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void on_signal(int sig) {
    const char msg[] = "docx-mcp: crashed (signal). Backtrace:\n";
    (void)!write(2, msg, sizeof msg - 1);
#if DOCX_HAVE_EXECINFO
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

}  // namespace

void install_crash_handler() {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(on_exception);
    // no "program stopped working" dialog box: a stdio server has no user at the screen
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) std::signal(sig, on_signal);
    // a client closing the pipe must not kill us mid-write; the write fails instead
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace docx::util

// Minimal test framework.
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> v; return v; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
inline void fail(const char* file, int line, const std::string& msg) {
    std::printf("  FAIL %s:%d: %s\n", file, line, msg.c_str());
    failures()++;
}
inline int run(int argc, char** argv) {
    int n = 0;
    for (auto& c : cases()) {
        if (argc > 1 && std::string(c.name).find(argv[1]) == std::string::npos) continue;
        int before = failures();
        try { c.fn(); } catch (std::exception& e) { fail(c.name, 0, std::string("exception: ") + e.what()); }
        std::printf("%s %s\n", failures() == before ? "ok  " : "FAIL", c.name);
        n++;
    }
    std::printf("%d tests, %d failures\n", n, failures());
    return failures() ? 1 : 0;
}
}  // namespace t

#define TEST(name) static void name(); static t::Reg reg_##name(#name, name); static void name()
#define CHECK(c) do { if (!(c)) t::fail(__FILE__, __LINE__, #c); } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) t::fail(__FILE__, __LINE__, std::string(#a " == " #b "  got: ") + ::t::show(_a) + " vs " + ::t::show(_b)); } while (0)
namespace t {
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return "\"" + std::string(s) + "\""; }
template <class T> std::string show(const T& v) { if constexpr (std::is_arithmetic_v<T>) return std::to_string(v); else return "?"; }
}

// Time one mutating call cycle on a large document: snapshot + edit + dedupe.
#include <chrono>
#include <cstdio>

#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/text.hpp"

using namespace docx::wml;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    auto d = Document::open(argv[1]);
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    auto t0 = clk::now();
    auto snap = d.to_bytes();
    auto t1 = clk::now();
    d.dedupe_ids();
    auto t2 = clk::now();
    auto r = Document::from_bytes(snap);
    auto t3 = clk::now();
    auto fast = d.snapshot();
    auto t4 = clk::now();
    auto r2 = Document::from_bytes(fast);
    auto t5 = clk::now();
    std::printf("%zu bytes: to_bytes %.1fms snapshot %.1fms dedupe %.1fms reopen %.1f/%.1fms\n", snap.size(), ms(t0, t1),
                ms(t3, t4), ms(t1, t2), ms(t2, t3), ms(t4, t5));
}

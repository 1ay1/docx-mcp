// Run an external program (no shell) with a timeout. Used to render pages
// through LibreOffice when it's installed.
#pragma once
#include <string>
#include <vector>

namespace docx::util {

struct ProcResult {
    int exit_code = -1;     // -1: couldn't start
    bool timed_out = false;
    std::string output;     // stdout + stderr, capped
};

// argv[0] is looked up on PATH when it has no slash. stdin is /dev/null.
ProcResult run_process(const std::vector<std::string>& argv, int timeout_ms);

// The first of `names` found on PATH (or the absolute paths), "" if none.
std::string find_program(const std::vector<std::string>& names);

}  // namespace docx::util

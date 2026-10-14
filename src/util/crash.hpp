// Crash reporting: on a fatal signal, print a backtrace to stderr so users
// (and CI) can report what happened. stdout is left alone (protocol only).
#pragma once

namespace docx::util {
void install_crash_handler();
}

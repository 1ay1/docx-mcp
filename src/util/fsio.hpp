// Portable file and path helpers. Paths are UTF-8 std::strings everywhere in
// the code; these convert at the OS boundary (wide APIs on Windows).
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

#include "util/util.hpp"

namespace docx::util {

// UTF-8 string -> filesystem path (correct on Windows for non-ASCII names).
std::filesystem::path u8path(std::string_view utf8);
// Path -> UTF-8 string.
std::string path_utf8(const std::filesystem::path& p);
// Expand a leading "~" and make absolute + normalized (does not need to exist).
std::string normalize_path(std::string_view utf8);

Bytes read_file(std::string_view utf8_path);
// Write atomically: temp file in the same directory, then replace. Retries
// briefly on Windows when another process holds the file open.
void write_file_atomic(std::string_view utf8_path, const Bytes& data);
bool file_exists(std::string_view utf8_path);
std::uintmax_t file_size(std::string_view utf8_path);

// Put stdin/stdout in binary mode (no CRLF translation on Windows).
void set_binary_stdio();
// Environment variable as UTF-8 ("" if unset).
std::string env(const char* name);

}  // namespace docx::util

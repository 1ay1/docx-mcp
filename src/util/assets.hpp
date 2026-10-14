#pragma once
#include <string>
#include <string_view>

namespace docx::assets {
// Contents of an embedded file under assets/, e.g. "word/blank/styles.xml".
// Throws if missing.
std::string_view get(std::string_view path);
}  // namespace docx::assets

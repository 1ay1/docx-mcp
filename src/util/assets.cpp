#include "util/assets.hpp"

#include <stdexcept>
#include <utility>

namespace docx::assets {
extern const std::pair<std::string_view, std::string_view> files[];
extern const unsigned n_files;

std::string_view get(std::string_view path) {
    for (unsigned i = 0; i < n_files; i++)
        if (files[i].first == path) return files[i].second;
    throw std::runtime_error("missing embedded asset " + std::string(path));
}
}  // namespace docx::assets

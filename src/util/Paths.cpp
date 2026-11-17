#include "util/Paths.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <unistd.h>

namespace proxy {
namespace {

std::string homeDirectory() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string();
}

} // namespace

std::string anchorPath(const std::string& path, const std::string& base_dir) {
    if (path.empty() || base_dir.empty()) return path;
    if (path[0] == '~' && (path.size() == 1 || path[1] == '/')) {
        const std::string home = homeDirectory();
        if (home.empty()) return path;
        return path.size() == 1 ? home : home + path.substr(1);
    }
    const std::filesystem::path p(path);
    if (p.is_absolute()) return p.lexically_normal().string();
    return (std::filesystem::path(base_dir) / p).lexically_normal().string();
}

std::string directoryOf(const std::string& file) {
    std::error_code ec;
    auto dir = std::filesystem::absolute(std::filesystem::path(file), ec);
    if (ec) return {};
    dir = dir.lexically_normal().parent_path();
    return dir.empty() ? std::string("/") : dir.string();
}

} // namespace proxy
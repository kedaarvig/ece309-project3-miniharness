// src/tools/workspace.cpp
//
// Workspace::resolve is purely textual: it never looks at the disk. Symlinks
// are check_symlinks' job (workspace_provided.cpp).

#include "tools/workspace.h"

#include <utility>

namespace fs = std::filesystem;

namespace {

bool reject(std::string* reason, std::string why) {
    *reason = std::move(why);
    return false;
}

}  // namespace

bool Workspace::resolve(std::string_view user_path, fs::path* out, std::string* reason) const {
    if (user_path.empty()) return reject(reason, "empty path");

    // A string_view can carry a NUL, but the OS would stop reading there.
    if (user_path.find('\0') != std::string_view::npos)
        return reject(reason, "path contains a NUL byte");

    const std::string shown(user_path);
    fs::path p{shown};
    if (p.is_absolute() || p.has_root_path())
        return reject(reason, "absolute paths are not allowed, use a path relative to the workspace: " + shown);

    // After this, a ".." can only be left at the very front.
    fs::path n = p.lexically_normal();
    if (n.empty() || n == ".") {
        *out = root_;
        return true;
    }
    if (*n.begin() == "..") return reject(reason, "path escapes the workspace: " + shown);

    *out = root_ / n;
    return true;
}

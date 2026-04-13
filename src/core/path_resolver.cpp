#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/utils/shell.hpp"
#include <algorithm>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>

namespace ai_mirror::core {

fs::path PathResolver::resolve(const fs::path& p) {
    if (p.empty()) return p;

    fs::path expanded = p;

    if (p.string().substr(0, 2) == "~/") {
        expanded = fs::path(utils::get_home_dir(utils::get_current_username())) / p.string().substr(2);
    } else if (p.string() == "~") {
        expanded = utils::get_home_dir(utils::get_current_username());
    }

    std::error_code ec;
    return fs::canonical(expanded, ec);
}

fs::path PathResolver::to_ai_user_path(const fs::path& main_path, const std::string& ai_user, const std::string& main_user) {
    std::string main_home = utils::get_home_dir(main_user);
    std::string ai_home = utils::get_home_dir(ai_user);

    auto main_str = main_path.string();
    if (main_str.substr(0, main_home.length()) == main_home) {
        std::string relative = main_str.substr(main_home.length());
        return fs::path(ai_home) / relative;
    }

    return main_path;
}

std::string PathResolver::detect_owner_user(const fs::path& p) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) {
        return "";
    }

    auto* pw = getpwuid(st.st_uid);
    return pw ? pw->pw_name : "";
}

std::optional<fs::path> PathResolver::find_project_root(const fs::path& p) {
    fs::path current = resolve(p);

    while (!current.empty() && current != "/") {
        if (fs::exists(current / ".git") || fs::exists(current / ".ai-mirror.toml")) {
            return current;
        }
        current = current.parent_path();
    }

    return std::nullopt;
}

bool PathResolver::is_under_project(const fs::path& path, const fs::path& project_root) {
    auto resolved_path = resolve(path);
    auto resolved_root = resolve(project_root);

    auto path_str = resolved_path.string();
    auto root_str = resolved_root.string();

    if (path_str.length() < root_str.length()) return false;
    return path_str.substr(0, root_str.length()) == root_str;
}

}

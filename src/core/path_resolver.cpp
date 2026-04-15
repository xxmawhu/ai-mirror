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

static bool username_valid(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') return false;
    }
    return true;
}

std::string PathResolver::detect_ai_user_from_path(const fs::path& p, const std::string& main_user, const std::string& prefix) {
    if (main_user.empty() || prefix.empty()) return "";

    std::string expected_prefix = prefix + main_user + "_";
    std::error_code ec;

    auto check_component = [&](const fs::path& component) -> std::string {
        std::string name = component.string();
        if (name.length() <= expected_prefix.length()) return "";
        if (name.substr(0, expected_prefix.length()) != expected_prefix) return "";
        if (!username_valid(name)) return "";
        fs::path candidate_home = "/home/" + name;
        if (fs::is_directory(candidate_home, ec) && !ec) {
            return name;
        }
        return "";
    };

    fs::path resolved = fs::canonical(p, ec);
    if (ec) {
        resolved = fs::weakly_canonical(p, ec);
        if (ec) return "";
    }
    std::string resolved_str = resolved.string();

    for (auto it = resolved.begin(); it != resolved.end(); ++it) {
        std::string result = check_component(*it);
        if (!result.empty()) return result;
    }

    fs::path parent = resolved;
    while (!parent.empty() && parent != "/") {
        auto component = parent.filename();
        std::string result = check_component(component);
        if (!result.empty()) return result;
        parent = parent.parent_path();
    }

    if (!resolved_str.empty()) {
        for (size_t i = 1; i < resolved_str.size(); ++i) {
            if (resolved_str[i] == '/') {
                std::string segment = resolved_str.substr(0, i);
                fs::path seg_path(segment);
                std::string seg_name = seg_path.filename().string();
                std::string result = check_component(seg_name);
                if (!result.empty()) return result;
            }
        }
    }

    return "";
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

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace ai_mirror::core {

class PathResolver {
public:
    static fs::path resolve(const fs::path& p);
    static fs::path to_ai_user_path(const fs::path& main_path, const std::string& ai_user, const std::string& main_user);
    static std::string detect_owner_user(const fs::path& p);
    static std::string detect_ai_user_from_path(const fs::path& p, const std::string& main_user, const std::string& prefix);
    static std::optional<fs::path> find_project_root(const fs::path& p);
    static bool is_under_project(const fs::path& path, const fs::path& project_root);
};

}

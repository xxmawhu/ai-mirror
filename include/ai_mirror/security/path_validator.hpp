#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::security {

struct PathCheckResult {
    bool safe;
    std::string reason;
};

PathCheckResult validate_mount_paths(const fs::path& source, const fs::path& target);
PathCheckResult validate_no_circular_mount(const fs::path& source, const fs::path& target);

bool is_subpath(const fs::path& parent, const fs::path& child);
fs::path safe_canonical(const fs::path& p);
bool validate_path_allowed(const fs::path& p);
bool validate_mount_source(const fs::path& source);
bool validate_path_exists(const fs::path& p);

}

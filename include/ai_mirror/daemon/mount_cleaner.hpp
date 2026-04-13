#pragma once

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::daemon {

class MountCleaner {
public:
    std::vector<fs::path> find_stale_mounts();
    int force_cleanup(const std::vector<fs::path>& mounts);
    int cleanup_for_user(const std::string& username);
};

}

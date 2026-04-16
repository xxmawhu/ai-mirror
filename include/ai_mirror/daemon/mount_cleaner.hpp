#pragma once

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::daemon {

// Mount cleanup utilities for stale bind mounts left by ai-mirror.
//
// Path matching precision:
// - find_stale_mounts() matches mount source paths exactly using /proc/self/mountinfo
// - cleanup_for_user() validates mount point path starts with exact user home prefix
//   (e.g., /home/imaxx_alpha exactly, not matching /home/imaxx_alpha_other_project)
// - Boundary check prevents prefix collision between ai-mirror users
//
// All path validation uses validate_path_allowed() from path_validator.hpp
// which rejects SYSTEM_DIRS blacklist (/etc, /root, /tmp, etc.).

class MountCleaner {
public:
    explicit MountCleaner(const std::string& user_prefix = "i");
    
    // Find stale bind mounts where source directory no longer exists.
    // Uses /proc/self/mountinfo to enumerate all mounts.
    // Mount source validated via lstat() to correctly handle symlink scenarios.
    std::vector<fs::path> find_stale_mounts();
    
    // Force unmount specified paths without username validation.
    // WARNING: Use with caution - validates SYSTEM_DIRS blacklist only.
    // Prefer cleanup_for_user() for safe user-scoped cleanup.
    int force_cleanup(const std::vector<fs::path>& mounts);
    
    // Cleanup all mounts for a specific ai-user with validation:
    // 1. Validates username via validate_ai_user_ownership()
    // 2. Checks mount point starts with user's home directory
    // 3. Ensures path boundary (no prefix collision)
    int cleanup_for_user(const std::string& username);
    
private:
    std::string prefix_;
};

}

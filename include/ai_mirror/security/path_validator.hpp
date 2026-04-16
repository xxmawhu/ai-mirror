#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::security {

// Path validation for security-sensitive operations.
//
// All validation functions reject paths that:
// - Are empty or contain ".." components
// - Fall under SYSTEM_DIRS blacklist (18 FHS directories)
// - Are symlinks that resolve outside allowed boundaries
//
// safe_canonical() returns empty path on failure (non-existent, dangling symlink),
// callers must treat empty as rejection. No weakly_canonical fallback to prevent
// ".." bypass attacks.
//
// validate_path_allowed() checks SYSTEM_DIRS blacklist only.
// is_path_allowed() in shell.cpp adds home directory prefix validation.

struct PathCheckResult {
    bool safe;
    std::string reason;
};

// Validate source and target paths for bind mount operations.
// Rejects empty paths, same paths, and circular mount relationships.
PathCheckResult validate_mount_paths(const fs::path& source, const fs::path& target);

// Check if target is under source or vice versa (circular mount detection).
PathCheckResult validate_no_circular_mount(const fs::path& source, const fs::path& target);

// Check if child path is under parent path (after canonical resolution).
bool is_subpath(const fs::path& parent, const fs::path& child);

// Resolve path to canonical form using fs::canonical().
// Returns empty path on failure (non-existent, symlink loop, permission denied).
// Callers must treat empty path as validation failure.
fs::path safe_canonical(const fs::path& p);

// Validate path is not under SYSTEM_DIRS blacklist.
// Does NOT validate home directory prefix - use is_path_allowed() for that.
bool validate_path_allowed(const fs::path& p);

// Validate mount source path (wrapper for validate_path_allowed).
bool validate_mount_source(const fs::path& source);

// Check if path exists and is a regular file or directory.
// Uses O_PATH|O_NOFOLLOW to avoid symlink following.
bool validate_path_exists(const fs::path& p);

// Safely create directory hierarchy using fd-based approach.
// Each component is opened with O_NOFOLLOW to prevent TOCTOU symlink races.
// mkdirat() creates final component, fails with EEXIST if already exists.
// Returns true on success or if path already exists.
bool safe_create_directories(const fs::path& p);

}

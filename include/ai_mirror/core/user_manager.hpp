#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

/// stat(2) info for a mount source path, persisted so mount_watch can detect
/// source changes without re-stating remote filesystems.
struct MountStatInfo {
  ino_t ino = 0;
  dev_t dev = 0;
  mode_t mode = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  off_t size = 0;
  time_t mtime = 0;
};

/// A single bind mount entry persisted in `.am_status`.
struct MountInfo {
  std::string source;
  std::string target;
  bool read_only = true;
  MountStatInfo source_stat; ///< stat(2) at time of mount/update
};

struct UserInfo {
  std::string username;
  std::string home_dir;
  std::string main_user;
  std::string project_path; // canonical absolute path of the project
  std::string path_hash;    // SHA256(project_path)[:6]
  uid_t uid;
  gid_t gid;
  bool exists;
  std::string error;
  std::vector<MountInfo> mounts; ///< persisted bind mount list
};

// AI user management with deterministic username generation.
//
// Username generation strategy:
// - Format: {prefix}{effective_user}_{path_hash[:6]}
// - path_hash: first 6 hex chars of SHA256(canonical absolute path)
// - Example: prefix="i", user="maxx", path="/home/maxx/ai-mirror"
//   → SHA256(...) = "a3f2b1...", username = "imaxx_a3f2b1"
// - Hash-based naming eliminates same-name project collision
// - Total username length <= 32 chars (Linux limit)
// - Collision (different paths, same 6-char hash) throws error
class UserManager {
public:
  explicit UserManager(const std::string &prefix,
                       const std::vector<fs::path> &allowed_bases = {});

  UserInfo create_ai_user(const std::string &project_path);
  bool remove_ai_user(const std::string &username, bool force = false);
  std::optional<UserInfo> get_user_info(const std::string &username) const;
  bool user_exists(const std::string &username) const;
  std::optional<std::string>
  derive_username(const fs::path &project_path) const;
  std::vector<UserInfo> list_ai_users() const;

  static std::optional<UserInfo> read_state(const fs::path &project_dir);

  // Update .am_status with current mount info for an AI user.
  // Reads /proc/mounts via Graft to get the actual mount list, stats each
  // source, then rewrites the status file preserving all existing fields.
  static bool update_state_mounts(const std::string &username,
                                  const fs::path &home_dir,
                                  const std::string &prefix);

  // Compute path hash for a canonical path (first 6 hex chars of SHA256)
  static std::string compute_path_hash(const fs::path &canonical_path);

  // Fix AM home directory permissions per root.md §2.3
  // AI user home MUST be 0755 (owner rwx, group/other r-x), NO g+w
  // Main user operates via SSH, not via shared group write permission
  static void fix_home_dir_permissions(const fs::path &home_dir,
                                       const std::string &main_user);

  std::string get_prefix() const { return prefix_; }

private:
  std::string prefix_;
  std::vector<fs::path> allowed_bases_;
  std::optional<std::string>
  generate_username(const fs::path &project_path) const;
  std::optional<std::string> compute_username(const fs::path &project_path,
                                              bool check_collision) const;
  bool execute_useradd(const std::string &username, const fs::path &home_dir,
                       uid_t uid, gid_t gid);
  bool execute_userdel(const std::string &username, bool remove_home);
};

} // namespace ai_mirror::core

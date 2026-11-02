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
  std::string fstype; ///< filesystem type (ext4, nfs4, proc, tmpfs, ...)
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
// Username/home strategy (current):
// - Format: ai_<hash>, hash = first 12 base36 chars of SHA256(canonical path)
// - Home: /home/ai_<hash>, created and owned by the AI user, mode 0755
// - The project directory stays at its original location and is separate from
//   the home; the AI user is granted write access to it.
//
// Legacy format (still readable and removable, never created):
// - Format: {prefix}{main_user}_{path_hash[:6]}, home = project directory
// - path_hash: first 6 hex chars of SHA256(canonical path)
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

  // Read .am_status from the current-layout home for a project path:
  // /home/ai_<hash>/.am_status where hash is derived from the canonical path.
  static std::optional<UserInfo>
  read_state_for_project(const fs::path &project_path);

  // Read .am_status directly from a layout home directory.  Used when the
  // project path is not known (e.g. force-destroy by username).
  static std::optional<UserInfo> read_state_from_home(const fs::path &home_dir);

  // True if `username` belongs to `main_user` under either layout.  The
  // current layout (ai_<hash>) carries no main user in the name, so ownership
  // is taken from the main_user field of its .am_status.
  static bool is_ai_user_of(const std::string &username,
                            const std::string &main_user,
                            const std::string &prefix);

  // Update .am_status with current mount info for an AI user.
  // Reads /proc/mounts via Graft to get the actual mount list, stats each
  // source, then rewrites the status file preserving all existing fields.
  static bool update_state_mounts(const std::string &username,
                                  const fs::path &home_dir,
                                  const std::string &prefix);

  // Rebuild .am_status from scratch using kernel mount state.
  // Used when the file is corrupted (empty, invalid JSON, missing fields) and
  // update_state_mounts() cannot proceed because it requires valid existing
  // state.  All field values are reconstructed from authoritative sources:
  //   - username, uid, gid, home_dir → /etc/passwd (by username)
  //   - main_user → extracted from username ({prefix}{main_user}_{hash})
  //   - path_hash → computed from project_path (SHA256 → first 6 hex)
  //   - mounts → /proc/mounts + /proc/self/mountinfo via Graft
  //   - project_path → caller-provided (from CLI argument)
  static bool rebuild_state(const fs::path &home_dir,
                            const std::string &username,
                            const std::string &main_user,
                            const fs::path &project_path,
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
  compute_username(const fs::path &project_path) const;
  bool execute_useradd(const std::string &username, const fs::path &home_dir,
                       uid_t uid, gid_t gid);
  bool execute_userdel(const std::string &username, bool remove_home);
};

} // namespace ai_mirror::core

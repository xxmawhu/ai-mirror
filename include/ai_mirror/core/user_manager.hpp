#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

struct UserInfo {
  std::string username;
  std::string home_dir;
  std::string main_user;
  uid_t uid;
  gid_t gid;
  bool exists;
  std::string error;
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

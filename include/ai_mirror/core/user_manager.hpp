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
    uid_t uid;
    gid_t gid;
    bool exists;
    std::string error;
};

// AI user management with deterministic username generation.
//
// Username generation strategy:
// - Format: {prefix}_{sanitized_project_name}_{hash_suffix}
// - sanitized_project_name: basename of project path, [a-z0-9_] only, truncated to 20 chars
// - hash_suffix: 8-char hex from /dev/urandom (cryptographic, not std::hash)
// - Underscore separator ensures no prefix collision between users
// - Total username length <= 32 chars (Linux limit)
//
// remove_ai_user() and list_ai_users() validate underscore separator
// to prevent prefix collision attacks (e.g., prefix_alice matching prefix_alice2).
class UserManager {
public:
    explicit UserManager(const std::string& prefix);

    UserInfo create_ai_user(const std::string& project_path);
    bool remove_ai_user(const std::string& username, bool force = false);
    std::optional<UserInfo> get_user_info(const std::string& username) const;
    bool user_exists(const std::string& username) const;
    std::optional<std::string> derive_username(const fs::path& project_path) const;
    std::vector<UserInfo> list_ai_users() const;

    static std::optional<UserInfo> read_state(const fs::path& project_dir);

    std::string get_prefix() const { return prefix_; }

private:
    std::string prefix_;
    std::optional<std::string> generate_username(const fs::path& project_path) const;
    std::optional<std::string> compute_username(const fs::path& project_path, bool check_collision) const;
    bool execute_useradd(const std::string& username, const fs::path& home_dir, uid_t uid, gid_t gid);
    bool execute_userdel(const std::string& username, bool remove_home);
};

}

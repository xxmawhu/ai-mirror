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
};

class UserManager {
public:
    explicit UserManager(const std::string& prefix);

    UserInfo create_ai_user(const std::string& project_path);
    bool remove_ai_user(const std::string& username, bool force = false);
    std::optional<UserInfo> get_user_info(const std::string& username) const;
    bool user_exists(const std::string& username) const;
    std::string derive_username(const std::string& project_path) const;
    std::vector<UserInfo> list_ai_users() const;

    std::string get_prefix() const { return prefix_; }

private:
    std::string prefix_;
    std::string generate_username(const fs::path& project_path) const;
    bool execute_useradd(const std::string& username, const fs::path& home_dir);
    bool execute_userdel(const std::string& username, bool remove_home);
};

}

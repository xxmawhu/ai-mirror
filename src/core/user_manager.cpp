#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <algorithm>
#include <random>
#include <sstream>
#include <pwd.h>
#include <grp.h>

namespace ai_mirror::core {

UserManager::UserManager(const std::string& prefix) : prefix_(prefix) {}

std::optional<std::string> UserManager::generate_username(const fs::path& project_path) const {
    std::string stem = project_path.filename().string();
    std::replace(stem.begin(), stem.end(), '.', '_');
    std::replace(stem.begin(), stem.end(), '-', '_');

    std::string base = prefix_ + utils::get_effective_username() + "_" + stem;
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const size_t max_len = 32;
    std::string username = base.substr(0, max_len);

    if (user_exists(username)) {
        utils::get_logger()->error(
            "Username collision detected: '{}' already exists (derived from path '{}'). "
            "Cannot create user - project path too similar to existing project.",
            username, project_path.string());
        return std::nullopt;
    }

    return username;
}

std::optional<std::string> UserManager::derive_username(const std::string& project_path) const {
    return generate_username(fs::path(project_path));
}

bool UserManager::execute_useradd(const std::string& username, const fs::path& home_dir) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username: {}", username);
        return false;
    }

    auto result = utils::exec_safe({"useradd",
        "--create-home",
        "--home-dir", home_dir.string(),
        "--shell", "/usr/sbin/nologin",
        "--comment", "ai-mirror managed user",
        username});
    if (result.exit_code != 0) {
        utils::get_logger()->error("useradd failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

bool UserManager::execute_userdel(const std::string& username, bool remove_home) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for deletion: {}", username);
        return false;
    }

    std::vector<std::string> args;
    args.reserve(4);
    args.push_back("userdel");
    if (remove_home) {
        args.push_back("--remove");
    }
    args.push_back(username);

    auto result = utils::exec_safe(args);
    if (result.exit_code != 0) {
        utils::get_logger()->error("userdel failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

UserInfo UserManager::create_ai_user(const std::string& project_path) {
    fs::path proj(project_path);

    if (!security::validate_path_allowed(proj)) {
        utils::get_logger()->error("Project path rejected (system directory): {}", proj.string());
        return {"", "", 0, 0, false};
    }

    std::string main_user = utils::get_effective_username();
    std::string main_home = utils::get_home_dir(main_user);
    if (!main_home.empty()) {
        std::string ps = fs::absolute(proj).string();
        if (ps.length() < main_home.length() || ps.substr(0, main_home.length()) != main_home) {
            utils::get_logger()->error("Project path must be under caller home ({}): {}", main_home, ps);
            return {"", "", 0, 0, false};
        }
    }

    auto username_opt = generate_username(proj);
    if (!username_opt) {
        return {"", "", 0, 0, false};
    }
    std::string username = std::move(*username_opt);

    if (user_exists(username)) {
        auto info = get_user_info(username);
        return info.value_or(UserInfo{username, "/home/" + username, 0, 0, true});
    }

    fs::path home_dir = "/home/" + username;

    if (!execute_useradd(username, home_dir)) {
        utils::get_logger()->error("Failed to create user: {}", username);
        return {username, home_dir.string(), 0, 0, false};
    }

    auto info = get_user_info(username);
    utils::get_logger()->info("Created ai-user: {} (uid={})", username, info->uid);
    return info.value_or(UserInfo{username, home_dir.string(), 0, 0, true});
}

bool UserManager::remove_ai_user(const std::string& username, bool force) {
    if (!user_exists(username)) {
        return false;
    }

    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for removal: {}", username);
        return false;
    }

    std::string prefix_check = prefix_ + utils::get_effective_username();
    if (username.substr(0, prefix_check.length()) != prefix_check) {
        utils::get_logger()->error("Refusing to remove non-ai-mirror user: {}", username);
        return false;
    }

    return execute_userdel(username, force);
}

std::optional<UserInfo> UserManager::get_user_info(const std::string& username) const {
    auto* pw = getpwnam(username.c_str());
    if (!pw) {
        return std::nullopt;
    }
    return UserInfo{username, pw->pw_dir, pw->pw_uid, pw->pw_gid, true};
}

bool UserManager::user_exists(const std::string& username) const {
    return getpwnam(username.c_str()) != nullptr;
}

std::vector<UserInfo> UserManager::list_ai_users() const {
    std::vector<UserInfo> users;
    std::string prefix_str = prefix_ + utils::get_effective_username();

    setpwent();
    while (auto* pw = getpwent()) {
        std::string name(pw->pw_name);
        if (name.substr(0, prefix_str.length()) == prefix_str) {
            users.push_back({name, pw->pw_dir, pw->pw_uid, pw->pw_gid, true});
        }
    }
    endpwent();

    return users;
}

}

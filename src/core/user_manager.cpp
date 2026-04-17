#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <algorithm>
#include <random>
#include <sstream>
#include <functional>
#include <fstream>
#include <array>
#include <iomanip>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <unistd.h>

namespace ai_mirror::core {

static std::string get_crypto_hash_suffix(const std::string& input) {
    unsigned char buf[8];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        utils::get_logger()->warn("Failed to open /dev/urandom for hash suffix, falling back to std::hash");
        size_t h = std::hash<std::string>{}(input);
        std::ostringstream oss;
        oss << std::hex << (h & 0xFFFF);
        std::string suffix = oss.str();
        while (suffix.size() < 4) suffix = "0" + suffix;
        return suffix;
    }
    ssize_t n = read(fd, buf, 2);
    close(fd);
    if (n != 2) {
        utils::get_logger()->warn("Failed to read /dev/urandom for hash suffix");
        size_t h = std::hash<std::string>{}(input);
        std::ostringstream oss;
        oss << std::hex << (h & 0xFFFF);
        std::string suffix = oss.str();
        while (suffix.size() < 4) suffix = "0" + suffix;
        return suffix;
    }
    unsigned short val = (buf[0] << 8) | buf[1];
    std::ostringstream oss;
    oss << std::hex << val;
    std::string suffix = oss.str();
    while (suffix.size() < 4) suffix = "0" + suffix;
    return suffix;
}

UserManager::UserManager(const std::string& prefix) : prefix_(prefix) {}

// Generates a unique ai-user username from project_path.  Strategy:
// 1. Extract filename stem, sanitize dots/hyphens to underscores
// 2. Combine prefix + main_user + "_" + stem, lowercase
// 3. Truncate stem to 20 chars to leave room for hash suffix
// 4. Append 4-char hex hash of full path (hash & 0xFFFF) as collision breaker
// 5. Final truncation to Linux username limit (32 chars)
// The hash suffix ensures uniqueness even when project names are truncated
// (e.g. "/home/alice/my-very-long-project-name-001" vs "...-002").
std::optional<std::string> UserManager::generate_username(const fs::path& project_path) const {
    std::string stem = project_path.filename().string();
    std::replace(stem.begin(), stem.end(), '.', '_');
    std::replace(stem.begin(), stem.end(), '-', '_');

    std::string base = prefix_ + utils::get_effective_username() + "_" + stem;
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string hash_suffix = get_crypto_hash_suffix(project_path.string());

    const size_t max_stem_len = 20;
    std::string truncated = base.substr(0, max_stem_len);
    std::string username = truncated + "_" + hash_suffix;

    if (username.size() > 32) {
        username = username.substr(0, 32);
    }

    if (user_exists(username)) {
        utils::get_logger()->error(
            "Username collision detected: '{}' already exists (derived from path '{}'). "
            "Cannot create user - project path too similar to existing project.",
            username, project_path.string());
        return std::nullopt;
    }

    return username;
}

std::optional<std::string> UserManager::derive_username(const fs::path& project_path) const {
    return generate_username(project_path);
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

    // Validate caller's home directory exists and project is under it.
    // An empty home directory means getpwnam() failed or pw_dir is NULL,
    // which would skip the path-below-home check below — a security bypass.
    std::string main_user = utils::get_effective_username();
    std::string main_home = utils::get_effective_home();
    if (main_home.empty()) {
        main_home = utils::get_home_dir(main_user);
    }
    if (main_home.empty()) {
        utils::get_logger()->error("Cannot determine home directory for user '{}', rejecting", main_user);
        return {"", "", 0, 0, false};
    }
    std::string ps = fs::absolute(proj).string();
    if (ps.length() < main_home.length() || ps.substr(0, main_home.length()) != main_home) {
        utils::get_logger()->error("Project path must be under caller home ({}): {}", main_home, ps);
        return {"", "", 0, 0, false};
    }

    auto username_opt = generate_username(proj);
    if (!username_opt) {
        return {"", "", 0, 0, false};
    }
    std::string username = std::move(*username_opt);

    if (user_exists(username)) {
        auto info = get_user_info(username);
        return info.value_or(UserInfo{username, proj.string(), 0, 0, true});
    }

    fs::path home_dir = proj;

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

    std::string prefix_check = prefix_ + utils::get_effective_username() + "_";
    if (username.length() <= prefix_check.length()
        || username.substr(0, prefix_check.length()) != prefix_check) {
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
    std::string prefix_str = prefix_ + utils::get_effective_username() + "_";

    setpwent();
    while (auto* pw = getpwent()) {
        std::string name(pw->pw_name);
        if (name.length() > prefix_str.length()
            && name.substr(0, prefix_str.length()) == prefix_str) {
            users.push_back({name, pw->pw_dir, pw->pw_uid, pw->pw_gid, true});
        }
    }
    endpwent();

    return users;
}

}

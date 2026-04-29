#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/unique_fd.hpp"
#include <algorithm>
#include <random>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>

namespace ai_mirror::core {

static const std::string STATE_FILE = ".am_status";

static std::string md5_hex(const std::string& input) {
    unsigned char hash[16];
    unsigned int hash_len = 0;
    EVP_Digest(input.c_str(), input.size(), hash, &hash_len, EVP_md5(), nullptr);
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

static std::string make_state_content(const UserInfo& info, const std::string& main_user) {
    // Build JSON via string concatenation, then PoW with us-level timestamp as nonce
    // Difficulty: hash must start with "000" (3 leading zeros)
    std::string base;
    base.reserve(256);
    base += "{\n";
    base += "  \"username\": \""; base += info.username; base += "\",\n";
    base += "  \"uid\": "; base += std::to_string(info.uid); base += ",\n";
    base += "  \"gid\": "; base += std::to_string(info.gid); base += ",\n";
    base += "  \"home_dir\": \""; base += info.home_dir; base += "\",\n";
    base += "  \"main_user\": \""; base += main_user; base += "\",\n";

    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();

    // PoW: try successive us timestamps until hash starts with "000"
    // Expected ~4096 attempts for 12-bit difficulty, negligible CPU/memory
    std::string pow_input;
    for (int64_t t = us; ; ++t) {
        pow_input = base;
        pow_input += "  \"timestamp\": "; pow_input += std::to_string(t); pow_input += ",\n";
        std::string h = md5_hex(pow_input);
        if (h.substr(0, 3) == "000") {
            pow_input += "  \"hash\": \""; pow_input += h; pow_input += "\"\n}\n";
            return pow_input;
        }
    }
}

static bool verify_state_content(const std::string& content) {
    nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded()) return false;

    // New PoW format: has "hash" field (also covers legacy timestamp+hash)
    // Verify by extracting content before the hash line and re-hashing
    if (j.contains("hash")) {
        auto pos = content.rfind("\"hash\"");
        if (pos == std::string::npos) return false;
        auto line_start = content.rfind('\n', pos);
        if (line_start == std::string::npos) line_start = 0;
        std::string pow_input = content.substr(0, line_start + 1);
        std::string pow_hash = md5_hex(pow_input);
        return pow_hash.substr(0, 3) == "000";
    }

    // Old PoW format (nonce-based): md5 of entire content must start with "00000"
    std::string h = md5_hex(content);
    return h.substr(0, 5) == "00000";
}

static bool write_state_file(const fs::path& home_dir, const UserInfo& info, const std::string& main_user) {
    fs::path state_path = home_dir / STATE_FILE;

    std::string content = make_state_content(info, main_user);

    utils::unique_fd ufd(::open(state_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (!ufd) {
        utils::get_logger()->error("Failed to write state file: {}", state_path.string());
        return false;
    }
    if (::write(ufd.get(), content.c_str(), content.size()) != static_cast<ssize_t>(content.size())) {
        utils::get_logger()->error("Failed to write state file: {}", state_path.string());
        return false;
    }
    return true;
}

static std::optional<UserInfo> read_state_file(const fs::path& home_dir) {
    fs::path state_path = home_dir / STATE_FILE;
    std::ifstream ifs(state_path);
    if (!ifs.is_open()) return std::nullopt;
    try {
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        if (!verify_state_content(content)) {
            utils::get_logger()->error("State file md5 verification failed: {}", state_path.string());
            return std::nullopt;
        }
        nlohmann::json j = nlohmann::json::parse(content);
        UserInfo info;
        info.username = j.value("username", "");
        info.uid = j.value("uid", 0);
        info.gid = j.value("gid", 0);
        info.home_dir = j.value("home_dir", "");
        info.main_user = j.value("main_user", "");
        info.exists = true;
        info.error = "";
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

static unsigned int compute_next_seq(uid_t base_uid) {
    unsigned int max_seq = 0;
    setpwent();
    while (auto* pw = getpwent()) {
        if (pw->pw_uid > base_uid * 10000u && pw->pw_uid < (base_uid + 1u) * 10000u) {
            unsigned int seq = pw->pw_uid - base_uid * 10000u;
            if (seq > max_seq) max_seq = seq;
        }
    }
    endpwent();
    return max_seq + 1;
}

UserManager::UserManager(const std::string& prefix) : prefix_(prefix) {}

std::optional<std::string> UserManager::compute_username(const fs::path& project_path, bool check_collision) const {
    std::string stem = project_path.filename().string();

    // Sanitize stem: replace characters not allowed in usernames with '-'
    // Allowed: [a-z0-9_-] — dots, spaces, and other chars become '-'
    std::string sanitized;
    for (char c : stem) {
        char lc = std::tolower(static_cast<unsigned char>(c));
        if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9') || lc == '_' || lc == '-') {
            sanitized += lc;
        } else {
            // Replace invalid char with '-', but avoid consecutive dashes
            if (!sanitized.empty() && sanitized.back() != '-') {
                sanitized += '-';
            }
        }
    }
    // Remove trailing dash
    while (!sanitized.empty() && sanitized.back() == '-') {
        sanitized.pop_back();
    }
    if (sanitized.empty()) {
        utils::get_logger()->error(
            "Project name '{}' produces empty username after sanitization. "
            "Please rename the project directory.",
            stem);
        return std::nullopt;
    }

    std::string base = prefix_ + utils::get_effective_username() + "_" + sanitized;
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string username = base.substr(0, 32);

    if (!utils::validate_username(username)) {
        utils::get_logger()->error(
            "Project name '{}' produces invalid username '{}' (sanitized from '{}'): "
            "must contain only [a-z0-9_-], no leading digit, max 32 chars. "
            "Please rename the project directory.",
            stem, username, sanitized);
        return std::nullopt;
    }

    if (check_collision && user_exists(username)) {
        utils::get_logger()->error(
            "Username collision: '{}' already exists for path '{}'",
            username, project_path.string());
        return std::nullopt;
    }

    return username;
}

std::optional<std::string> UserManager::generate_username(const fs::path& project_path) const {
    return compute_username(project_path, true);
}

std::optional<std::string> UserManager::derive_username(const fs::path& project_path) const {
    return compute_username(project_path, false);
}

bool UserManager::execute_useradd(const std::string& username, const fs::path& home_dir, uid_t uid, gid_t gid) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username: {}", username);
        return false;
    }

    auto grp_result = utils::exec_safe({"groupadd", "--gid", std::to_string(gid), username});
    if (grp_result.exit_code != 0) {
        if (grp_result.stderr_output.find("already exists") == std::string::npos) {
            utils::get_logger()->warn("groupadd warning: {}", grp_result.stderr_output);
        }
    }

    auto result = utils::exec_safe({"useradd",
        "--uid", std::to_string(uid),
        "--gid", std::to_string(gid),
        "--home-dir", home_dir.string(),
        "--shell", "/bin/bash",
        "--comment", "ai-mirror managed user",
        username});
    if (result.exit_code != 0) {
        utils::get_logger()->error("useradd failed: {}", result.stderr_output);
        return false;
    }

    auto unlock = utils::exec_safe({"passwd", "-u", username});
    if (unlock.exit_code != 0) {
        utils::exec_safe({"usermod", "-p", "", username});
    }

    return true;
}

bool UserManager::execute_userdel(const std::string& username, bool remove_home) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for deletion: {}", username);
        return false;
    }

    // Capture GID before userdel removes the user entry
    gid_t user_gid = static_cast<gid_t>(-1);
    struct passwd* pw = getpwnam(username.c_str());
    if (pw) {
        user_gid = pw->pw_gid;
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
        if (result.stderr_output.find("mail spool") != std::string::npos
            && result.stderr_output.find("not found") != std::string::npos) {
            utils::get_logger()->info("userdel: mail spool not found (expected for ai-users): {}", username);
        } else {
            utils::get_logger()->error("userdel failed: {}", result.stderr_output);
            return false;
        }
    }

    // Delete the user's primary group (groupadd'd in execute_useradd)
    if (user_gid != static_cast<gid_t>(-1)) {
        auto grp_result = utils::exec_safe({"groupdel", username});
        if (grp_result.exit_code != 0) {
            // Not fatal: group may have been auto-removed by userdel, or already gone
            utils::get_logger()->info("groupdel {}: {} (may already be removed)", username, grp_result.stderr_output);
        } else {
            utils::get_logger()->info("Removed group: {} (gid={})", username, user_gid);
        }
    }

    return true;
}

UserInfo UserManager::create_ai_user(const std::string& project_path) {
    fs::path proj(project_path);

    std::string main_user = utils::get_effective_username();

    if (!utils::is_path_allowed(proj, main_user)) {
        std::string err = "Project path not allowed for user '" + main_user + "': " + proj.string();
        utils::get_logger()->error("{}", err);
        return {"", "", "", 0, 0, false, err};
    }

    std::string main_home = utils::get_effective_home();
    if (main_home.empty()) {
        main_home = utils::get_home_dir(main_user);
    }
    if (main_home.empty()) {
        std::string err = "Cannot determine home directory for user '" + main_user + "'";
        utils::get_logger()->error("{}", err);
        return {"", "", "", 0, 0, false, err};
    }

    std::error_code ec;
    fs::path abs_proj = fs::canonical(proj, ec);
    if (ec) {
        abs_proj = fs::weakly_canonical(fs::absolute(proj), ec);
        if (ec) {
            std::string err = "Cannot resolve project path: " + proj.string();
            utils::get_logger()->error("{}", err);
            return {"", "", "", 0, 0, false, err};
        }
    }

    std::string main_home_canon;
    fs::path mh = fs::canonical(main_home, ec);
    if (!ec) {
        main_home_canon = mh.string();
    } else {
        main_home_canon = fs::weakly_canonical(main_home).string();
    }
    if (main_home_canon.empty()) {
        std::string err = "Cannot canonicalize home directory: " + main_home;
        utils::get_logger()->error("{}", err);
        return {"", "", "", 0, 0, false, err};
    }

    std::string ps = abs_proj.string();
    if (ps.length() < main_home_canon.length()
        || ps.substr(0, main_home_canon.length()) != main_home_canon
        || (ps.length() > main_home_canon.length() && ps[main_home_canon.length()] != '/')) {
        std::string err = "Project path must be under caller home (" + main_home_canon + "): " + ps;
        utils::get_logger()->error("{}", err);
        return {"", "", "", 0, 0, false, err};
    }

    auto state = read_state_file(abs_proj);
    if (state) {
        if (user_exists(state->username)) {
            auto sys_info = get_user_info(state->username);
            if (sys_info && sys_info->uid == state->uid && sys_info->gid == state->gid) {
                utils::get_logger()->info("User already exists (state file matches): {} (uid={})",
                    state->username, state->uid);
                return *state;
            }
            utils::get_logger()->warn("State file exists but system uid/gid mismatch, will recreate");
        }
    }

    auto username_opt = generate_username(proj);
    if (!username_opt) {
        auto derived = derive_username(proj);
        if (derived && user_exists(*derived)) {
            auto existing = get_user_info(*derived);
            if (existing) {
                write_state_file(abs_proj, *existing, main_user);
                utils::get_logger()->info("Recovered existing user: {} (uid={}), wrote state file",
                    existing->username, existing->uid);
                return *existing;
            }
        }
        std::string err = "Cannot create valid username for project '" + proj.string()
            + "': directory name must contain only [a-z0-9_-], no leading digit, max 32 chars";
        return {"", "", "", 0, 0, false, err};
    }
    std::string username = std::move(*username_opt);

    if (user_exists(username)) {
        auto info = get_user_info(username);
        if (!info) {
            utils::get_logger()->error("User '{}' exists but getpwnam failed", username);
            return {username, proj.string(), "", 0, 0, false, "getpwnam lookup failed"};
        }
        write_state_file(abs_proj, *info, main_user);
        utils::get_logger()->info("User already exists: {} (uid={}), wrote state file",
            info->username, info->uid);
        return *info;
    }

    fs::path home_dir = proj;
    uid_t base_uid = utils::get_login_uid();
    if (base_uid == 0) base_uid = getuid();
    unsigned int seq = compute_next_seq(base_uid);
    uid_t new_uid = base_uid * 10000u + seq;
    gid_t new_gid = new_uid;

    if (!execute_useradd(username, home_dir, new_uid, new_gid)) {
        std::string err = "useradd failed for '" + username + "': see logs for details";
        utils::get_logger()->error("{}", err);
        return {username, home_dir.string(), "", new_uid, new_gid, false, err};
    }

    auto info = get_user_info(username);
    if (!info) {
        UserInfo created{username, home_dir.string(), "", new_uid, new_gid, true, ""};
        write_state_file(home_dir, created, main_user);
        utils::get_logger()->info("Created ai-user: {} (uid={})", username, new_uid);
        return created;
    }

    write_state_file(home_dir, *info, main_user);
    utils::get_logger()->info("Created ai-user: {} (uid={})", username, info->uid);
    return *info;
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
    return UserInfo{username, pw->pw_dir, "", pw->pw_uid, pw->pw_gid, true, {}};
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
            users.push_back({name, pw->pw_dir, "", pw->pw_uid, pw->pw_gid, true, ""});
        }
    }
    endpwent();

    return users;
}

std::optional<UserInfo> UserManager::read_state(const fs::path& project_dir) {
    return read_state_file(project_dir);
}

}

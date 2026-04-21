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
#include <nlohmann/json.hpp>

#include <openssl/evp.h>

namespace ai_mirror::core {

static const std::string STATE_FILE = ".am_status";

static std::string sha256_hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_Digest(input.c_str(), input.size(), hash, &hash_len, EVP_sha256(), nullptr);
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

static bool verify_state_hash(const nlohmann::json& j) {
    if (!j.contains("nonce") || !j["nonce"].is_string()) return false;
    nlohmann::json copy = j;
    copy.erase("hash");
    std::string serialized = copy.dump();
    std::string h = sha256_hex(serialized);
    return h.substr(0, 4) == "0000";
}

static nlohmann::json make_state_json(const UserInfo& info, const std::string& main_user) {
    nlohmann::json j;
    j["username"] = info.username;
    j["uid"] = info.uid;
    j["gid"] = info.gid;
    j["home_dir"] = info.home_dir;
    j["main_user"] = main_user;

    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dist(0, 0xFFFFFFFFu);
    for (int attempt = 0; attempt < 1000000; ++attempt) {
        j["nonce"] = dist(rd);
        std::string serialized = j.dump();
        std::string h = sha256_hex(serialized);
        if (h.substr(0, 4) == "0000") {
            j["hash"] = h;
            return j;
        }
    }
    j["nonce"] = 0;
    std::string serialized = j.dump();
    j["hash"] = sha256_hex(serialized);
    return j;
}

static bool write_state_file(const fs::path& home_dir, const UserInfo& info, const std::string& main_user) {
    fs::path state_path = home_dir / STATE_FILE;

    nlohmann::json j = make_state_json(info, main_user);

    utils::unique_fd ufd(::open(state_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (!ufd) {
        utils::get_logger()->error("Failed to write state file: {}", state_path.string());
        return false;
    }
    std::string content = j.dump(2) + "\n";
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
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (!verify_state_hash(j)) {
            utils::get_logger()->error("State file hash verification failed: {}", state_path.string());
            return std::nullopt;
        }
        UserInfo info;
        info.username = j.value("username", "");
        info.uid = j.value("uid", 0);
        info.gid = j.value("gid", 0);
        info.home_dir = j.value("home_dir", "");
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
    std::replace(stem.begin(), stem.end(), '.', '_');
    std::replace(stem.begin(), stem.end(), '-', '_');

    std::string base = prefix_ + utils::get_effective_username() + "_" + stem;
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string username = base.substr(0, 32);

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
        if (result.stderr_output.find("mail spool") != std::string::npos
            && result.stderr_output.find("not found") != std::string::npos) {
            utils::get_logger()->info("userdel: mail spool not found (expected for ai-users): {}", username);
            return true;
        }
        utils::get_logger()->error("userdel failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

UserInfo UserManager::create_ai_user(const std::string& project_path) {
    fs::path proj(project_path);

    std::string main_user = utils::get_effective_username();

    if (!utils::is_path_allowed(proj, main_user)) {
        std::string err = "Project path not allowed for user '" + main_user + "': " + proj.string();
        utils::get_logger()->error("{}", err);
        return {"", "", 0, 0, false, err};
    }

    std::string main_home = utils::get_effective_home();
    if (main_home.empty()) {
        main_home = utils::get_home_dir(main_user);
    }
    if (main_home.empty()) {
        std::string err = "Cannot determine home directory for user '" + main_user + "'";
        utils::get_logger()->error("{}", err);
        return {"", "", 0, 0, false, err};
    }

    std::error_code ec;
    fs::path abs_proj = fs::canonical(proj, ec);
    if (ec) {
        abs_proj = fs::weakly_canonical(fs::absolute(proj), ec);
        if (ec) {
            std::string err = "Cannot resolve project path: " + proj.string();
            utils::get_logger()->error("{}", err);
            return {"", "", 0, 0, false, err};
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
        return {"", "", 0, 0, false, err};
    }

    std::string ps = abs_proj.string();
    if (ps.length() < main_home_canon.length()
        || ps.substr(0, main_home_canon.length()) != main_home_canon
        || (ps.length() > main_home_canon.length() && ps[main_home_canon.length()] != '/')) {
        std::string err = "Project path must be under caller home (" + main_home_canon + "): " + ps;
        utils::get_logger()->error("{}", err);
        return {"", "", 0, 0, false, err};
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
            get_user_info(*derived);
            utils::get_logger()->info("User already exists for project: {} ({})",
                *derived, proj.string());
        }
        std::string err = "Username collision for project: " + proj.string();
        return {"", "", 0, 0, false, err};
    }
    std::string username = std::move(*username_opt);

    if (user_exists(username)) {
        auto info = get_user_info(username);
        if (!info) {
            utils::get_logger()->error("User '{}' exists but getpwnam failed", username);
            return {username, proj.string(), 0, 0, false, "getpwnam lookup failed"};
        }
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
        return {username, home_dir.string(), new_uid, new_gid, false, err};
    }

    auto info = get_user_info(username);
    if (!info) {
        UserInfo created{username, home_dir.string(), new_uid, new_gid, true, ""};
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
    return UserInfo{username, pw->pw_dir, pw->pw_uid, pw->pw_gid, true, {}};
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
            users.push_back({name, pw->pw_dir, pw->pw_uid, pw->pw_gid, true, ""});
        }
    }
    endpwent();

    return users;
}

}

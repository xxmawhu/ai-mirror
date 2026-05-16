#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/unique_fd.hpp"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace ai_mirror::core {

static const std::string STATE_FILE = ".am_status";

static std::string sha256_hex(const std::string &input) {
  unsigned char hash[32];
  unsigned int hash_len = 0;
  EVP_Digest(input.c_str(), input.size(), hash, &hash_len, EVP_sha256(),
             nullptr);
  std::ostringstream oss;
  for (unsigned int i = 0; i < hash_len; ++i) {
    oss << std::hex << std::setfill('0') << std::setw(2)
        << static_cast<int>(hash[i]);
  }
  return oss.str();
}

static std::string md5_hex(const std::string &input) {
  unsigned char hash[16];
  unsigned int hash_len = 0;
  EVP_Digest(input.c_str(), input.size(), hash, &hash_len, EVP_md5(), nullptr);
  std::ostringstream oss;
  for (unsigned int i = 0; i < hash_len; ++i) {
    oss << std::hex << std::setfill('0') << std::setw(2)
        << static_cast<int>(hash[i]);
  }
  return oss.str();
}

static std::string make_state_content(const UserInfo &info,
                                      const std::string &main_user) {
  // Build JSON via string concatenation, then PoW with us-level timestamp as
  // nonce Difficulty: hash of entire content must start with "000" (3 leading
  // zeros) NO hash field is stored - the PoW is verified by checking
  // md5(content) prefix
  std::string base;
  base.reserve(256);
  base += "{\n";
  base += "  \"username\": \"";
  base += info.username;
  base += "\",\n";
  base += "  \"uid\": ";
  base += std::to_string(info.uid);
  base += ",\n";
  base += "  \"gid\": ";
  base += std::to_string(info.gid);
  base += ",\n";
  base += "  \"home_dir\": \"";
  base += info.home_dir;
  base += "\",\n";
  base += "  \"main_user\": \"";
  base += main_user;
  base += "\",\n";
  base += "  \"project_path\": \"";
  base += info.project_path;
  base += "\",\n";
  base += "  \"path_hash\": \"";
  base += info.path_hash;
  base += "\",\n";

  auto now = std::chrono::system_clock::now();
  auto epoch = now.time_since_epoch();
  auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();

  // PoW: try successive us timestamps until md5 of entire content starts with
  // "000" Expected ~4096 attempts for 12-bit difficulty, negligible CPU/memory
  for (int64_t t = us;; ++t) {
    std::string content = base;
    content += "  \"timestamp\": ";
    content += std::to_string(t);
    content += "\n}\n";
    std::string h = md5_hex(content);
    if (h.substr(0, 3) == "000") {
      return content; // Return content WITHOUT hash field
    }
  }
}

static bool verify_state_content(const std::string &content) {
  // PoW verification: md5 of content must start with "000"
  // Supports three formats:
  //   - Legacy hash format: has "hash" field → stored hash starts with "000"
  //   - New PoW format: has "project_path"/"path_hash" → md5(content) starts with "000"
  //   - Old format: no "hash" AND no "project_path"/"path_hash" → trust directly (pre-PoW)
  auto j = nlohmann::json::parse(content, nullptr, false);
  if (j.is_discarded())
    return false;

  // Legacy hash format: verify stored hash starts with "000"
  if (j.contains("hash")) {
    std::string stored_hash = j["hash"].get<std::string>();
    return stored_hash.substr(0, 3) == "000";
  }

  // New PoW format: has project_path and path_hash fields
  if (j.contains("project_path") || j.contains("path_hash")) {
    return md5_hex(content).substr(0, 3) == "000";
  }

  // Old format (pre-PoW): trust directly - these files were created before PoW
  // was introduced, they only have username/uid/gid/home_dir/main_user/timestamp
  return true;
}

static bool write_state_file(const fs::path &home_dir, const UserInfo &info,
                             const std::string &main_user) {
  fs::path state_path = home_dir / STATE_FILE;

  std::string content = make_state_content(info, main_user);

  utils::unique_fd ufd(
      ::open(state_path.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644));
  if (!ufd) {
    utils::get_logger()->error("Failed to write state file: {}",
                               state_path.string());
    return false;
  }
  if (::write(ufd.get(), content.c_str(), content.size()) !=
      static_cast<ssize_t>(content.size())) {
    utils::get_logger()->error("Failed to write state file: {}",
                               state_path.string());
    return false;
  }
  return true;
}

static std::optional<UserInfo> read_state_file(const fs::path &home_dir) {
  fs::path state_path = home_dir / STATE_FILE;
  std::ifstream ifs(state_path);
  if (!ifs.is_open())
    return std::nullopt;
  try {
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ifs.close();
    if (content.empty())
      return std::nullopt;

    // Quick JSON check before expensive verify
    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded()) {
      utils::get_logger()->error("State file is not valid JSON: {}",
                                 state_path.string());
      return std::nullopt;
    }

    if (!verify_state_content(content)) {
      utils::get_logger()->error("State file md5 verification failed: {}",
                                 state_path.string());
      return std::nullopt;
    }

    UserInfo info;
    info.username = j.value("username", "");
    info.uid = j.value("uid", 0);
    info.gid = j.value("gid", 0);
    info.home_dir = j.value("home_dir", "");
    info.main_user = j.value("main_user", "");
    info.project_path = j.value("project_path", "");
    info.path_hash = j.value("path_hash", "");
    info.exists = true;
    info.error = "";

    // Backward compatibility: if project_path/path_hash missing, derive from
    // username Username format: {prefix}{user}_{hash6} → extract hash6 as
    // path_hash
    if (info.path_hash.empty() && !info.username.empty()) {
      // Find last underscore and extract 6-char hash suffix
      auto last_underscore = info.username.rfind('_');
      if (last_underscore != std::string::npos &&
          info.username.length() - last_underscore - 1 == 6) {
        info.path_hash = info.username.substr(last_underscore + 1);
        utils::get_logger()->debug(
            "Derived path_hash '{}' from username '{}' (legacy format)",
            info.path_hash, info.username);
      }
    }
    // project_path defaults to home_dir if not stored
    if (info.project_path.empty()) {
      info.project_path = info.home_dir;
    }

    // Verify ownership: project directory owner must match uid in state file
    struct stat st;
    if (stat(home_dir.c_str(), &st) == 0) {
      if (st.st_uid != info.uid) {
        utils::get_logger()->error(
            "Ownership mismatch: {} owner uid={} but state file claims uid={}",
            home_dir.string(), st.st_uid, info.uid);
        return std::nullopt;
      }
    }

    // Migrate legacy format: if hash field exists, rewrite to new format
    if (j.contains("hash")) {
      utils::get_logger()->info(
          "Migrating legacy state file format (removing hash field): {}",
          state_path.string());

      // Build new content without hash field
      std::string new_content = make_state_content(info, info.main_user);

      // Write new state file
      utils::unique_fd ufd(
          ::open(state_path.c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644));
      if (ufd) {
        if (::write(ufd.get(), new_content.c_str(), new_content.size()) ==
            static_cast<ssize_t>(new_content.size())) {
          utils::get_logger()->info("State file migrated successfully: {}",
                                    state_path.string());
        } else {
          utils::get_logger()->warn("Failed to write migrated state file: {}",
                                    state_path.string());
        }
      }
    }

    return info;
  } catch (...) {
    return std::nullopt;
  }
}

static unsigned int compute_next_seq(uid_t base_uid) {
  unsigned int max_seq = 0;
  setpwent();
  while (auto *pw = getpwent()) {
    if (pw->pw_uid > base_uid * 10000u &&
        pw->pw_uid < (base_uid + 1u) * 10000u) {
      unsigned int seq = pw->pw_uid - base_uid * 10000u;
      if (seq > max_seq)
        max_seq = seq;
    }
  }
  endpwent();
  return max_seq + 1;
}

std::string UserManager::compute_path_hash(const fs::path &canonical_path) {
  return sha256_hex(canonical_path.string()).substr(0, 6);
}

UserManager::UserManager(const std::string &prefix,
                         const std::vector<fs::path> &allowed_bases)
    : prefix_(prefix), allowed_bases_(allowed_bases) {}

std::optional<std::string>
UserManager::compute_username(const fs::path &project_path,
                              bool check_collision) const {
  // Get canonical absolute path for stable hash
  std::error_code ec;
  fs::path canonical_path = fs::canonical(project_path, ec);
  if (ec) {
    utils::get_logger()->error("Cannot resolve canonical path for '{}': {}",
                               project_path.string(), ec.message());
    return std::nullopt;
  }

  // Compute SHA256 hash of canonical path, take first 6 hex chars
  std::string path_hash = sha256_hex(canonical_path.string()).substr(0, 6);

  // Username format: prefix + effective_user + "_" + path_hash[:6]
  std::string username =
      prefix_ + utils::get_effective_username() + "_" + path_hash;
  std::transform(username.begin(), username.end(), username.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Validate username format (should always pass with this scheme)
  if (!utils::validate_username(username)) {
    utils::get_logger()->error(
        "Generated username '{}' is invalid. "
        "This should never happen with hash-based naming.",
        username);
    return std::nullopt;
  }

  // Collision detection: if username exists for different path, throw error
  if (check_collision && user_exists(username)) {
    // Check if existing user belongs to same canonical path
    auto existing_info = get_user_info(username);
    if (existing_info) {
      fs::path existing_home = existing_info->home_dir;
      // Home dir format: /home/<username> or configured base
      // We cannot directly recover original path from hash, but collision
      // means: Two different paths produced same 6-char hash → extremely rare
      utils::get_logger()->error(
          "Username collision detected: '{}' already exists (home: {}). "
          "Path '{}' and another path share the same hash prefix '{}'. "
          "This is a hash collision (probability ~1/16^6). "
          "Please choose a different project path or remove existing AI user.",
          username, existing_home.string(), canonical_path.string(), path_hash);
      return std::nullopt;
    }
  }

  return username;
}

std::optional<std::string>
UserManager::generate_username(const fs::path &project_path) const {
  return compute_username(project_path, true);
}

std::optional<std::string>
UserManager::derive_username(const fs::path &project_path) const {
  return compute_username(project_path, false);
}

bool UserManager::execute_useradd(const std::string &username,
                                  const fs::path &home_dir, uid_t uid,
                                  gid_t gid) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username: {}", username);
    return false;
  }

  auto grp_result =
      utils::exec_safe({"groupadd", "--gid", std::to_string(gid), username});
  if (grp_result.exit_code != 0) {
    if (grp_result.stderr_output.find("already exists") == std::string::npos) {
      utils::get_logger()->warn("groupadd warning: {}",
                                grp_result.stderr_output);
    }
  }

  auto result = utils::exec_safe(
      {"useradd", "--uid", std::to_string(uid), "--gid", std::to_string(gid),
       "--home-dir", home_dir.string(), "--shell", "/bin/bash", "--comment",
       "ai-mirror managed user", username});
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

bool UserManager::execute_userdel(const std::string &username,
                                  bool remove_home) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username for deletion: {}", username);
    return false;
  }

  // Capture GID before userdel removes the user entry
  gid_t user_gid = static_cast<gid_t>(-1);
  struct passwd *pw = getpwnam(username.c_str());
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
    if (result.stderr_output.find("mail spool") != std::string::npos &&
        result.stderr_output.find("not found") != std::string::npos) {
      utils::get_logger()->info(
          "userdel: mail spool not found (expected for ai-users): {}",
          username);
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
      utils::get_logger()->info("groupdel {}: {} (may already be removed)",
                                username, grp_result.stderr_output);
    } else {
      utils::get_logger()->info("Removed group: {} (gid={})", username,
                                user_gid);
    }
  }

  return true;
}

UserInfo UserManager::create_ai_user(const std::string &project_path) {
  fs::path proj(project_path);

  std::string main_user = utils::get_effective_username();

  if (!utils::is_path_allowed(proj, main_user, allowed_bases_)) {
    std::string err = "Project path not allowed for user '" + main_user +
                      "': " + proj.string();
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err};
  }

  std::string main_home = utils::get_effective_home();
  if (main_home.empty()) {
    main_home = utils::get_home_dir(main_user);
  }
  if (main_home.empty()) {
    std::string err =
        "Cannot determine home directory for user '" + main_user + "'";
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err};
  }

  std::error_code ec;
  fs::path abs_proj = fs::canonical(proj, ec);
  if (ec) {
    abs_proj = fs::weakly_canonical(fs::absolute(proj), ec);
    if (ec) {
      std::string err = "Cannot resolve project path: " + proj.string();
      utils::get_logger()->error("{}", err);
      return {"", "", "", "", "", 0, 0, false, err};
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
    return {"", "", "", "", "", 0, 0, false, err};
  }

  std::string ps = abs_proj.string();

  // Check project path permissions using is_path_allowed
  // This handles owner, group, and other permissions properly
  if (!utils::is_path_allowed(abs_proj, main_user, allowed_bases_)) {
    std::string err = "Project path not accessible by user: " + ps;
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err};
  }

  // Also check SYSTEM_DIRS blacklist
  if (!security::validate_path_allowed(abs_proj)) {
    std::string err = "Project path in system directory (not allowed): " + ps;
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err};
  }

  auto state = read_state_file(abs_proj);
  if (state) {
    if (user_exists(state->username)) {
      auto sys_info = get_user_info(state->username);
      if (sys_info && sys_info->uid == state->uid &&
          sys_info->gid == state->gid) {
        utils::get_logger()->info(
            "User already exists (state file matches): {} (uid={})",
            state->username, state->uid);
        return *state;
      }
      utils::get_logger()->warn(
          "State file exists but system uid/gid mismatch, will recreate");
    }
  }

  auto username_opt = generate_username(proj);
  if (!username_opt) {
    auto derived = derive_username(proj);
    if (derived && user_exists(*derived)) {
      auto existing = get_user_info(*derived);
      if (existing) {
        write_state_file(abs_proj, *existing, main_user);
        utils::get_logger()->info(
            "Recovered existing user: {} (uid={}), wrote state file",
            existing->username, existing->uid);
        return *existing;
      }
    }
    std::string err = "Cannot create valid username for project '" +
                      proj.string() +
                      "': directory name must contain only [a-z0-9_-], no "
                      "leading digit, max 32 chars";
    return {"", "", "", "", "", 0, 0, false, err};
  }
  std::string username = std::move(*username_opt);
  std::string path_hash = compute_path_hash(abs_proj);
  std::string project_path_str = abs_proj.string();

  if (user_exists(username)) {
    auto info = get_user_info(username);
    if (!info) {
      utils::get_logger()->error("User '{}' exists but getpwnam failed",
                                 username);
      return {username, proj.string(),           "", "", "", 0, 0,
              false,    "getpwnam lookup failed"};
    }
    // Fill project_path and path_hash for existing user
    info->project_path = project_path_str;
    info->path_hash = path_hash;
    write_state_file(abs_proj, *info, main_user);
    utils::get_logger()->info(
        "User already exists: {} (uid={}), wrote state file", info->username,
        info->uid);
    return *info;
  }

  fs::path home_dir = proj;
  uid_t base_uid = utils::get_login_uid();
  if (base_uid == 0)
    base_uid = getuid();
  unsigned int seq = compute_next_seq(base_uid);
  uid_t new_uid = base_uid * 10000u + seq;
  gid_t new_gid = new_uid;

  if (!execute_useradd(username, home_dir, new_uid, new_gid)) {
    std::string err =
        "useradd failed for '" + username + "': see logs for details";
    utils::get_logger()->error("{}", err);
    return {username,  home_dir.string(), "",      project_path_str,
            path_hash, new_uid,           new_gid, false,
            err};
  }

  auto info = get_user_info(username);
  if (!info) {
    UserInfo created{username,  home_dir.string(), "",      project_path_str,
                     path_hash, new_uid,           new_gid, true,
                     ""};
    write_state_file(home_dir, created, main_user);
    utils::get_logger()->info("Created ai-user: {} (uid={})", username,
                              new_uid);
    return created;
  }

  write_state_file(home_dir, *info, main_user);
  utils::get_logger()->info("Created ai-user: {} (uid={})", username,
                            info->uid);
  return *info;
}

bool UserManager::remove_ai_user(const std::string &username, bool force) {
  if (!user_exists(username)) {
    return false;
  }

  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username for removal: {}", username);
    return false;
  }

  std::string prefix_check = prefix_ + utils::get_effective_username() + "_";
  if (username.length() <= prefix_check.length() ||
      username.substr(0, prefix_check.length()) != prefix_check) {
    utils::get_logger()->error("Refusing to remove non-ai-mirror user: {}",
                               username);
    return false;
  }

  return execute_userdel(username, force);
}

std::optional<UserInfo>
UserManager::get_user_info(const std::string &username) const {
  auto *pw = getpwnam(username.c_str());
  if (!pw) {
    return std::nullopt;
  }
  return UserInfo{username,   pw->pw_dir, "",   "", "",
                  pw->pw_uid, pw->pw_gid, true, {}};
}

bool UserManager::user_exists(const std::string &username) const {
  return getpwnam(username.c_str()) != nullptr;
}

std::vector<UserInfo> UserManager::list_ai_users() const {
  std::vector<UserInfo> users;
  std::string prefix_str = prefix_ + utils::get_effective_username() + "_";

  setpwent();
  while (auto *pw = getpwent()) {
    std::string name(pw->pw_name);
    if (name.length() > prefix_str.length() &&
        name.substr(0, prefix_str.length()) == prefix_str) {
      UserInfo info{name,       pw->pw_dir, "",   "", "",
                    pw->pw_uid, pw->pw_gid, true, ""};
      // Try to read state file for project_path and path_hash
      auto state = read_state_file(pw->pw_dir);
      if (state) {
        info.project_path = state->project_path;
        info.path_hash = state->path_hash;
        info.main_user = state->main_user;
      }
      users.push_back(info);
    }
  }
  endpwent();

  return users;
}

std::optional<UserInfo> UserManager::read_state(const fs::path &project_dir) {
  return read_state_file(project_dir);
}

} // namespace ai_mirror::core

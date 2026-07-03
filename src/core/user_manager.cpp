#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/vfs_util.hpp"
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
#include <sstream>
#include <sys/stat.h>
#include <thread>
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

static nlohmann::ordered_json build_source_stat_json(const MountStatInfo &st) {
  return {{"ino", st.ino},    {"dev", st.dev}, {"mode", st.mode},
          {"uid", st.uid},    {"gid", st.gid}, {"size", st.size},
          {"mtime", st.mtime}};
}

static nlohmann::ordered_json
make_mounts_json(const std::vector<MountInfo> &mounts) {
  auto arr = nlohmann::ordered_json::array();
  for (const auto &m : mounts) {
    nlohmann::ordered_json mj;
    mj["source"] = m.source;
    mj["target"] = m.target;
    mj["fstype"] = m.fstype;
    mj["read_only"] = m.read_only;
    // source_stat: include when either:
    // 1. Not a virtual filesystem (fstype+source combo is real), OR
    // 2. source is a real path (starts with '/') and stat data is
    //    populated (non-zero ino) — handles virtual filesystems
    //    (BeeGFS) whose source has been resolved to a real path by
    //    update_state_mounts.
    bool skip_stat = is_virtual_source(m.source, m.fstype);
    if (skip_stat && !m.source.empty() && m.source[0] == '/') {
      // Virtual fstype with a real path source — stat was done by
      // update_state_mounts after mountinfo resolution. Include
      // source_stat only if it has actual data.
      skip_stat = (m.source_stat.ino == 0);
    }
    if (!skip_stat) {
      mj["source_stat"] = build_source_stat_json(m.source_stat);
    }
    arr.push_back(std::move(mj));
  }
  return arr;
}

static std::string make_state_content(const UserInfo &info,
                                      const std::string &main_user) {
  // Build JSON fresh without timestamp — PoW (md5 nonce) was removed because
  // it had no security value and caused am update to reject valid files when
  // the md5 prefix didn't match (e.g., after am-mount-watch rewrote the file).
  nlohmann::ordered_json j;
  j["username"] = info.username;
  j["uid"] = info.uid;
  j["gid"] = info.gid;
  j["home_dir"] = info.home_dir;
  j["main_user"] = main_user;
  j["project_path"] = info.project_path;
  j["path_hash"] = info.path_hash;
  j["mounts"] = make_mounts_json(info.mounts);
  return j.dump(2) + "\n";
}

void UserManager::fix_home_dir_permissions(const fs::path &home_dir,
                                           const std::string &main_user) {
  // [root.md §2.3] AI user home MUST be 0755 (owner rwx, group/other r-x)
  // NO group write permission (g+w) - main user operates via SSH, not via
  // shared group write

  // 1. Set home_dir to 0755 (remove group write if present)
  auto chmod_result = utils::exec_safe({"chmod", "0755", home_dir.string()});
  if (chmod_result.exit_code != 0) {
    // [log-review] 降级为 warning: chmod 失败不影响 ai-user 正常使用
    utils::get_logger()->warn("Failed to set permissions 0755 on {}: {}",
                              home_dir.string(), chmod_result.stderr_output);
  } else {
    utils::get_logger()->info(
        "Set permissions 0755 on {} (AI user home isolation, per root.md §2.3)",
        home_dir.string());
  }

  // 2. chgrp to main user's primary group (for read access via group
  // membership)
  struct passwd *main_pw = getpwnam(main_user.c_str());
  if (main_pw) {
    struct group *main_grp = getgrgid(main_pw->pw_gid);
    std::string main_group_name =
        main_grp ? main_grp->gr_name : std::to_string(main_pw->pw_gid);
    auto chgrp_result =
        utils::exec_safe({"chgrp", main_group_name, home_dir.string()});
    if (chgrp_result.exit_code != 0) {
      // [log-review] 降级为 warning: chgrp 失败不影响 ai-user 正常使用
      utils::get_logger()->warn("Failed to chgrp '{}' to '{}': {}",
                                home_dir.string(), main_group_name,
                                chgrp_result.stderr_output);
    } else {
      utils::get_logger()->info(
          "Changed group of '{}' to '{}' (main user can read via group)",
          home_dir.string(), main_group_name);
    }
  }
}

// Validate that .am_status fields are safe for root-level operations.
// This runs BEFORE any chown/chgrp/mount operations that use these values.
// An attacker who gains write access to .am_status could set home_dir="/"
// and trigger a root chgrp/chmod on system directories.
static bool verify_state_safe(const nlohmann::json &j,
                              const fs::path &state_dir) {
  // 1. username must be non-empty
  auto username = j.value("username", "");
  if (username.empty()) {
    utils::get_logger()->error("verify: .am_status has empty 'username'");
    return false;
  }

  // 2. home_dir must match the directory containing .am_status
  //    Prevents home_dir="/" or home_dir="/etc" attacks
  auto home_dir = j.value("home_dir", "");
  if (home_dir.empty()) {
    utils::get_logger()->error("verify: .am_status has empty 'home_dir'");
    return false;
  }
  if (fs::weakly_canonical(home_dir) != fs::weakly_canonical(state_dir)) {
    utils::get_logger()->error(
        "verify: .am_status 'home_dir' mismatch: file says '{}', actual '{}'",
        home_dir, state_dir.string());
    return false;
  }

  // 3. username must exist in /etc/passwd
  struct passwd *pw = getpwnam(username.c_str());
  if (!pw) {
    utils::get_logger()->error("verify: AI user '{}' not found in /etc/passwd",
                               username);
    return false;
  }

  // 4. uid/gid in file must match passwd
  uid_t file_uid = j.value("uid", uid_t(0));
  gid_t file_gid = j.value("gid", gid_t(0));
  if (file_uid != pw->pw_uid || file_gid != pw->pw_gid) {
    utils::get_logger()->error(
        "verify: uid/gid mismatch for '{}': file={}/{} passwd={}/{}", username,
        file_uid, file_gid, pw->pw_uid, pw->pw_gid);
    return false;
  }

  // 5. main_user must exist in /etc/passwd
  auto main_user = j.value("main_user", "");
  if (!main_user.empty()) {
    struct passwd *main_pw = getpwnam(main_user.c_str());
    if (!main_pw) {
      utils::get_logger()->error(
          "verify: main_user '{}' not found in /etc/passwd", main_user);
      return false;
    }
  }

  return true;
}

static bool write_state_file(const fs::path &home_dir, const UserInfo &info,
                             const std::string &main_user) {
  fs::path state_path = home_dir / STATE_FILE;

  // Use atomic write: write to temp file, then rename.
  // Crash during write leaves the original file intact instead of truncating
  // it to empty (risk of O_TRUNC + crash).
  fs::path tmp_path = state_path;
  tmp_path += ".tmp";

  std::string content = make_state_content(info, main_user);

  utils::unique_fd ufd(
      ::open(tmp_path.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644));
  if (!ufd) {
    utils::get_logger()->error("Failed to write state file (tmp): {}",
                               tmp_path.string());
    return false;
  }
  if (::write(ufd.get(), content.c_str(), content.size()) !=
      static_cast<ssize_t>(content.size())) {
    utils::get_logger()->error("Failed to write state file (tmp): {}",
                               tmp_path.string());
    ::unlink(tmp_path.c_str());
    return false;
  }

  // State file stays root:root (0644, world-readable).
  // All writers (am via sudo, am-mount-watch as root systemd) run as root.
  // AI user never needs to write .am_status directly — all access is through
  // `am` CLI which elevates via sudo.  Keeping ownership root prevents
  // tampering by AI user.
  // fd closed by unique_fd destructor

  // Atomic rename: replace target atomically on the same filesystem.
  // Other readers see either the old file or the new file, never a
  // truncated/empty file.
  if (::rename(tmp_path.c_str(), state_path.c_str()) != 0) {
    utils::get_logger()->error("Failed to rename state file: {} -> {}: {}",
                               tmp_path.string(), state_path.string(),
                               strerror(errno));
    ::unlink(tmp_path.c_str());
    return false;
  }

  return true;
}

static std::optional<UserInfo> read_state_file(const fs::path &home_dir) {
  fs::path state_path = home_dir / STATE_FILE;

  // Open with O_NOFOLLOW to prevent symlink attacks: if .am_status has been
  // replaced with a symlink to an arbitrary file (e.g. /etc/shadow), open
  // fails (ELOOP) instead of following and reading attacker-controlled data.
  // Also limit to a reasonable max size (32MB) to prevent OOM on maliciously
  // inflated files.
  static constexpr size_t MAX_STATE_SIZE = 32 * 1024 * 1024; // 32 MiB
  utils::unique_fd ufd(
      ::open(state_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!ufd)
    return std::nullopt;

  // Read file content with size limit
  std::string content;
  content.resize(4096);
  ssize_t n;
  size_t total = 0;
  while ((n = ::read(ufd.get(), content.data() + total,
                     content.size() - total - 1)) > 0) {
    total += static_cast<size_t>(n);
    if (total >= MAX_STATE_SIZE) {
      utils::get_logger()->error("State file too large (>32 MiB): {}",
                                 state_path.string());
      return std::nullopt;
    }
    content.resize(content.size() * 2);
  }
  content.resize(total);

  if (content.empty()) {
    return std::nullopt;
  }

  // Parse JSON with exception to get detailed error info
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(content);
  } catch (const nlohmann::json::parse_error &e) {
    // Show exact byte position and brief message
    auto err_msg = fmt::format("JSON parse error at byte {}: {}", e.byte,
                               e.what());
    utils::get_logger()->error("State file is corrupted: {} — {}",
                               state_path.string(), err_msg);
    return std::nullopt;
  }

  // Validate critical fields before using them in root-level operations
  if (!verify_state_safe(j, home_dir)) {
    utils::get_logger()->error("State file rejected (unsafe fields): {}",
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

  // Parse mounts array (backward compatible: missing mounts field → empty)
  if (j.contains("mounts") && j["mounts"].is_array()) {
    for (const auto &mj : j["mounts"]) {
      MountInfo mi;
      mi.source = mj.value("source", "");
      mi.target = mj.value("target", "");
      mi.fstype = mj.value("fstype", "");
      mi.read_only = mj.value("read_only", true);
      if (mj.contains("source_stat")) {
        const auto &sj = mj["source_stat"];
        mi.source_stat.ino = sj.value("ino", ino_t(0));
        mi.source_stat.dev = sj.value("dev", dev_t(0));
        mi.source_stat.mode = sj.value("mode", mode_t(0));
        mi.source_stat.uid = sj.value("uid", uid_t(0));
        mi.source_stat.gid = sj.value("gid", gid_t(0));
        mi.source_stat.size = sj.value("size", off_t(0));
        mi.source_stat.mtime = sj.value("mtime", time_t(0));
      }
      info.mounts.push_back(std::move(mi));
    }
  }

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
    // [P0-path-hash] Fallback: compute path_hash from project_path if still
    // empty. This handles cases where username suffix is not exactly 6 chars
    // (e.g., new-style usernames with different formats).
    if (info.path_hash.empty() && !info.project_path.empty()) {
      info.path_hash = sha256_hex(info.project_path).substr(0, 6);
      utils::get_logger()->debug(
          "Computed path_hash '{}' from project_path '{}'", info.path_hash,
          info.project_path);
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
          utils::get_logger()->error("Failed to write migrated state file: {}",
                                     state_path.string());
        }
      }
    }

  return info;
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

  // [root.md §2.3] AI user home MUST be 0755 (owner rwx, group/other r-x)
  // NO group write permission (g+w) - main user operates via SSH, not via
  // shared group write
  auto chmod_result = utils::exec_safe({"chmod", "0755", home_dir.string()});
  if (chmod_result.exit_code != 0) {
    // [log-review] 降级为 warning: chmod 失败不影响 ai-user 正常使用
    // - home_dir 可能已不存在（useradd 失败的罕见场景）
    // - 文件系统可能不支持权限设置
    // - 权限可能已被设置为 0755（重复执行）
    utils::get_logger()->warn("Failed to set permissions 0755 on {}: {}",
                              home_dir.string(), chmod_result.stderr_output);
  } else {
    utils::get_logger()->info(
        "Set permissions 0755 on {} (AI user home isolation, per root.md §2.3)",
        home_dir.string());
  }

  auto unlock = utils::exec_safe({"passwd", "-u", username});
  if (unlock.exit_code != 0) {
    utils::exec_safe({"usermod", "-p", "", username});
  }

  // Add the new AI user to the ai-mirror group so it can execute `am` commands
  auto group_add = utils::exec_safe({"usermod", "-aG", "ai-mirror", username});
  if (group_add.exit_code != 0) {
    // [log-review] 降级为 warning: ai-mirror 组可能不存在（首次安装未完成时）
    utils::get_logger()->warn("Failed to add '{}' to ai-mirror group: {}",
                              username, group_add.stderr_output);
  } else {
    utils::get_logger()->info("Added '{}' to ai-mirror group", username);
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

  // Kill all processes owned by this user before userdel.
  // This prevents "userdel: user is currently used by process" errors.
  // Retry up to 3 times with pkill in between to handle persistent processes
  // (e.g. gpg-agent --supervised, systemd --user).
  const int retry_limit = 3;
  const int kill_sleep_sec = 3;
  for (int attempt = 1; attempt <= retry_limit; ++attempt) {
    // Kill remaining processes before this attempt
    auto kill_result = utils::exec_safe({"pkill", "-u", username});
    if (kill_result.exit_code == 0) {
      utils::get_logger()->info("Killed processes for user {} (attempt {})",
                                username, attempt);
    } else if (attempt == 1) {
      utils::get_logger()->debug(
          "pkill for user {} returned {} (may have no processes)", username,
          kill_result.exit_code);
    }

    // Sleep to allow processes to terminate
    std::this_thread::sleep_for(std::chrono::seconds(kill_sleep_sec));

    std::vector<std::string> args;
    args.reserve(4);
    args.push_back("userdel");
    if (remove_home) {
      args.push_back("--remove");
    }
    args.push_back(username);

    auto result = utils::exec_safe(args);
    if (result.exit_code == 0) {
      // Success — proceed to group cleanup
      goto group_cleanup;
    }

    // User already gone — treat as success
    if (result.stderr_output.find("does not exist") != std::string::npos) {
      utils::get_logger()->info("User {} does not exist, skipping", username);
      return true;
    }

    // Mail spool warning is non-fatal, proceed
    if (result.stderr_output.find("mail spool") != std::string::npos &&
        result.stderr_output.find("not found") != std::string::npos) {
      goto group_cleanup;
    }

    if (attempt < retry_limit) {
      utils::get_logger()->warn(
          "userdel attempt {} failed for user {}: {}. Retrying...", attempt,
          username, result.stderr_output);
    } else {
      utils::get_logger()->error(
          "userdel failed after {} attempts for user {}: {}", retry_limit,
          username, result.stderr_output);
      return false;
    }
  }

group_cleanup:
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
    return {"", "", "", "", "", 0, 0, false, err, {}};
  }

  std::string main_home = utils::get_effective_home();
  if (main_home.empty()) {
    main_home = utils::get_home_dir(main_user);
  }
  if (main_home.empty()) {
    std::string err =
        "Cannot determine home directory for user '" + main_user + "'";
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err, {}};
  }

  std::error_code ec;
  fs::path abs_proj = fs::canonical(proj, ec);
  if (ec) {
    abs_proj = fs::weakly_canonical(fs::absolute(proj), ec);
    if (ec) {
      std::string err = "Cannot resolve project path: " + proj.string();
      utils::get_logger()->error("{}", err);
      return {"", "", "", "", "", 0, 0, false, err, {}};
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
    return {"", "", "", "", "", 0, 0, false, err, {}};
  }

  std::string ps = abs_proj.string();

  // Check project path permissions using is_path_allowed
  // This handles owner, group, and other permissions properly
  if (!utils::is_path_allowed(abs_proj, main_user, allowed_bases_)) {
    std::string err = "Project path not accessible by user: " + ps;
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err, {}};
  }

  // Also check SYSTEM_DIRS blacklist (skip for root user)
  uid_t login_uid = utils::get_login_uid();
  if (login_uid == 0)
    login_uid = geteuid();
  if (login_uid != 0 && !security::validate_path_allowed(abs_proj)) {
    std::string err = "Project path in system directory (not allowed): " + ps;
    utils::get_logger()->error("{}", err);
    return {"", "", "", "", "", 0, 0, false, err, {}};
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
        // Fix permissions for existing AM home so main user can create
        // sub-projects
        fix_home_dir_permissions(abs_proj, main_user);
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
        // Fix permissions for existing AM home so main user can create
        // sub-projects
        fix_home_dir_permissions(abs_proj, main_user);
        return *existing;
      }
    }
    std::string err = "Cannot create valid username for project '" +
                      proj.string() +
                      "': directory name must contain only [a-z0-9_-], no "
                      "leading digit, max 32 chars";
    return {"", "", "", "", "", 0, 0, false, err, {}};
  }
  std::string username = std::move(*username_opt);
  std::string path_hash = compute_path_hash(abs_proj);
  std::string project_path_str = abs_proj.string();

  if (user_exists(username)) {
    auto info = get_user_info(username);
    if (!info) {
      utils::get_logger()->error("User '{}' exists but getpwnam failed",
                                 username);
      return {username,
              proj.string(),
              "",
              "",
              "",
              0,
              0,
              false,
              "getpwnam lookup failed",
              {}};
    }
    // Fill project_path and path_hash for existing user
    info->project_path = project_path_str;
    info->path_hash = path_hash;
    write_state_file(abs_proj, *info, main_user);
    utils::get_logger()->info(
        "User already exists: {} (uid={}), wrote state file", info->username,
        info->uid);
    // Fix permissions for existing AM home so main user can create
    // sub-projects
    fix_home_dir_permissions(abs_proj, main_user);
    return *info;
  }

  fs::path home_dir = abs_proj;
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
    return {username,  home_dir.string(),
            "",        project_path_str,
            path_hash, new_uid,
            new_gid,   false,
            err,       {}};
  }

  // Change home_dir group to main user's primary group and add group write
  // so that main user can create sub-projects inside AM home
  fix_home_dir_permissions(home_dir, main_user);

  auto info = get_user_info(username);
  if (!info) {
    UserInfo created{username,  home_dir.string(),
                     "",        project_path_str,
                     path_hash, new_uid,
                     new_gid,   true,
                     "",        {}};
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
                  pw->pw_uid, pw->pw_gid, true, "", {}};
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
                    pw->pw_uid, pw->pw_gid, true, "", {}};
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

bool UserManager::update_state_mounts(const std::string &username,
                                      const fs::path &home_dir,
                                      const std::string &prefix) {
  // 1. Read existing state
  auto info_opt = read_state_file(home_dir);
  if (!info_opt) {
    utils::get_logger()->error(
        "update_state_mounts: cannot read .am_status for {} in {}", username,
        home_dir.string());
    return false;
  }

  // 2. Get current mount list via Graft (reads /proc/mounts → device + fstype)
  Graft graft(prefix);
  auto mount_entries = graft.list_mounts(username);

  // 3. Read /proc/self/mountinfo to get the REAL source path.
  //    /proc/mounts column 1 (device) shows "beegfs_nodev" for all BeeGFS
  //    mounts — this is the FUSE device name, not the actual source path.
  //    /proc/self/mountinfo field 4 (root) contains the ORIGINAL source
  //    path of a bind mount, relative to the filesystem root:
  //      beegfs_nodev /target/path/.bashrc beegfs ...
  //      mountinfo field 4 (root) = /usr/maxx/.bashrc  ← REAL source!
  //    Full source = filesystem_root_mount + root_field
  //    = /mnt/beegfs_data + /usr/maxx/.bashrc
  //    = /mnt/beegfs_data/usr/maxx/.bashrc
  //
  //    We parse mountinfo once, walk parent IDs to find the filesystem root
  //    mount point, and construct the absolute source path for each target.
  std::unordered_map<std::string, std::string>
      mountinfo_source; // target→source
  {
    std::ifstream mi("/proc/self/mountinfo");
    if (mi.is_open()) {
      // Parse all mountinfo entries into lookup tables
      struct MiEntry {
        int id, parent;
        std::string root, mount, fstype;
      };
      std::unordered_map<int, MiEntry> entries;
      std::string line;
      while (std::getline(mi, line)) {
        std::istringstream iss(line);
        MiEntry e;
        std::string dummy, src;
        iss >> e.id >> e.parent >> dummy >> e.root >> e.mount;
        while (iss >> dummy && dummy != "-") {
        }
        iss >> e.fstype >> src;
        entries[e.id] = std::move(e);
      }
      // For each entry, find the filesystem root (root="/") by walking parents
      for (auto &[id, e] : entries) {
        if (e.root == "/")
          continue; // skip root mounts themselves
        // Walk up to find the root mount (same fstype)
        std::string fs_root;
        int pid = e.parent;
        for (int i = 0; i < 10 && pid > 0; i++) {
          auto pit = entries.find(pid);
          if (pit == entries.end())
            break;
          if (pit->second.root == "/") {
            fs_root = pit->second.mount;
            break;
          }
          pid = pit->second.parent;
        }
        if (!fs_root.empty() && !e.root.empty() && e.root[0] == '/') {
          // Construct the real source path
          mountinfo_source[e.mount] = fs_root + e.root;
        }
      }
    }
  }

  // 4. Convert MountEntry → MountInfo with real source and stat
  std::vector<MountInfo> mounts;
  for (const auto &me : mount_entries) {
    MountInfo mi;
    mi.source = me.source.string();
    mi.target = me.target.string();
    mi.fstype = me.fstype;
    mi.read_only = me.read_only;

    // For virtual filesystems (BeeGFS), /proc/mounts only shows the FUSE
    // device name.  Get the REAL source path from /proc/self/mountinfo
    // field 4 (root), then stat() it for real inode/dev/size.
    if (is_virtual_source(mi.source, mi.fstype)) {
      auto sit = mountinfo_source.find(mi.target);
      if (sit != mountinfo_source.end()) {
        mi.source = sit->second; // real source path
        // Keep fstype for serialization so mount_watch can identify
        // virtual filesystems; make_mounts_json already includes
        // source_stat when source is a real path (starts with '/').
        // No longer cleared — was previously cleared to let
        // make_mounts_json include source_stat, but the JSON
        // serializer now handles this correctly.
        struct stat st;
        if (::stat(mi.source.c_str(), &st) == 0) {
          mi.source_stat.ino = st.st_ino;
          mi.source_stat.dev = st.st_dev;
          mi.source_stat.mode = st.st_mode;
          mi.source_stat.uid = st.st_uid;
          mi.source_stat.gid = st.st_gid;
          mi.source_stat.size = st.st_size;
          mi.source_stat.mtime = st.st_mtime;
        }
      }
    } else {
      struct stat st;
      if (::stat(me.source.c_str(), &st) == 0) {
        mi.source_stat.ino = st.st_ino;
        mi.source_stat.dev = st.st_dev;
        mi.source_stat.mode = st.st_mode;
        mi.source_stat.uid = st.st_uid;
        mi.source_stat.gid = st.st_gid;
        mi.source_stat.size = st.st_size;
        mi.source_stat.mtime = st.st_mtime;
      }
    }
    mounts.push_back(std::move(mi));
  }
  info_opt->mounts = std::move(mounts);

  // 4. Rewrite state file with updated mounts
  if (!write_state_file(home_dir, *info_opt, info_opt->main_user)) {
    utils::get_logger()->error(
        "update_state_mounts: failed to write .am_status for {}", username);
    return false;
  }

  utils::get_logger()->info(
      "update_state_mounts: updated {} mount(s) in .am_status for {}",
      info_opt->mounts.size(), username);
  return true;
}

bool UserManager::rebuild_state(const fs::path &home_dir,
                                const std::string &username,
                                const std::string &main_user,
                                const fs::path &project_path,
                                const std::string &prefix) {
  // 1. Lookup uid/gid from /etc/passwd
  struct passwd *pw = getpwnam(username.c_str());
  if (!pw) {
    utils::get_logger()->error("rebuild_state: user '{}' not found in /etc/passwd",
                               username);
    return false;
  }

  // 2. Build UserInfo from authoritative sources
  UserInfo info;
  info.username = username;
  info.uid = pw->pw_uid;
  info.gid = pw->pw_gid;
  info.home_dir = home_dir.string();
  info.main_user = main_user;
  info.project_path = project_path.string();
  info.path_hash = compute_path_hash(project_path);
  info.exists = true;

  // 3. Get mounts from kernel via Graft
  Graft graft(prefix);
  auto mount_entries = graft.list_mounts(username);
  for (const auto &me : mount_entries) {
    MountInfo mi;
    mi.source = me.source.string();
    mi.target = me.target.string();
    mi.fstype = me.fstype;
    mi.read_only = me.read_only;
    struct stat st;
    if (::stat(me.source.c_str(), &st) == 0) {
      mi.source_stat.ino = st.st_ino;
      mi.source_stat.dev = st.st_dev;
      mi.source_stat.mode = st.st_mode;
      mi.source_stat.uid = st.st_uid;
      mi.source_stat.gid = st.st_gid;
      mi.source_stat.size = st.st_size;
      mi.source_stat.mtime = st.st_mtime;
    }
    info.mounts.push_back(std::move(mi));
  }

  // 4. Atomic write
  if (!write_state_file(home_dir, info, main_user)) {
    utils::get_logger()->error("rebuild_state: failed to write .am_status for {}",
                               username);
    return false;
  }

  // 5. Verify the written file is readable
  auto verify = read_state_file(home_dir);
  if (!verify) {
    utils::get_logger()->error(
        "rebuild_state: wrote .am_status for {} but read-back failed", username);
    return false;
  }

  utils::get_logger()->info(
      "rebuild_state: recovered .am_status for {} ({} mounts, path_hash={})",
      username, info.mounts.size(), info.path_hash);
  return true;
}

} // namespace ai_mirror::core

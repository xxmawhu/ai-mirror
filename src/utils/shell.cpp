#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ai_mirror::utils {

static constexpr size_t PIPE_BUF_SIZE = 4096;
// Maximum bytes to read from a subprocess pipe (10MB).  Prevents memory
// exhaustion if a malicious or buggy subprocess produces infinite output
// (e.g. `while true; do echo x; done`).  Once exceeded, the read is truncated
// and a warning logged; the caller receives partial output rather than OOM.
static constexpr size_t MAX_READ_SIZE = 10 * 1024 * 1024;

static std::string read_fd(int fd) {
  std::string result;
  result.reserve(PIPE_BUF_SIZE);
  std::array<char, PIPE_BUF_SIZE> buf{};
  ssize_t n;
  while ((n = ::read(fd, buf.data(), buf.size() - 1)) > 0) {
    buf.data()[n] = '\0';
    result += buf.data();
    if (result.size() > MAX_READ_SIZE) {
      get_logger()->warn("read_fd: output exceeded {} bytes, truncating",
                         MAX_READ_SIZE);
      break;
    }
  }
  return result;
}

static std::string resolve_command(const std::string &cmd) {
  if (cmd.empty())
    return "";

  static const std::map<std::string, std::string> COMMAND_PATHS = {
      {"mount", "/usr/bin/mount"},
      {"umount", "/usr/bin/umount"},
      {"chmod", "/usr/bin/chmod"},
      {"chown", "/usr/bin/chown"},
      {"chgrp", "/usr/bin/chgrp"},
      {"useradd", "/usr/sbin/useradd"},
      {"userdel", "/usr/sbin/userdel"},
      {"groupadd", "/usr/sbin/groupadd"},
      {"groupdel", "/usr/sbin/groupdel"},
      {"usermod", "/usr/sbin/usermod"},
      {"passwd", "/usr/bin/passwd"},
      {"gpasswd", "/usr/bin/gpasswd"},
      {"ssh-keygen", "/usr/bin/ssh-keygen"},
      {"mkdir", "/usr/bin/mkdir"},
      {"cp", "/usr/bin/cp"},
      {"mv", "/usr/bin/mv"},
      {"getent", "/usr/bin/getent"},
      {"findmnt", "/usr/bin/findmnt"},
      {"which", "/usr/bin/which"},
      {"ssh", "/usr/bin/ssh"},
      {"ps", "/usr/bin/ps"},
      {"kill", "/usr/bin/kill"},
      {"pkill", "/usr/bin/pkill"},
      {"pgrep", "/usr/bin/pgrep"},
  };

  if (cmd.find('/') != std::string::npos) {
    return cmd;
  }

  auto it = COMMAND_PATHS.find(cmd);
  if (it != COMMAND_PATHS.end()) {
    return it->second;
  }

  return "";
}

static ShellResult do_fork_exec(const std::string &file, char *const argv[],
                                int timeout_sec = EXEC_SAFE_TIMEOUT_DEFAULT) {
  int stdout_pipe[2];
  int stderr_pipe[2];

  if (::pipe(stdout_pipe) != 0) {
    return {-1, "", "pipe() failed for stdout"};
  }
  if (::pipe(stderr_pipe) != 0) {
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    return {-1, "", "pipe() failed for stderr"};
  }

  std::string resolved = resolve_command(file);

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[0]);
    ::close(stderr_pipe[1]);
    return {-1, "", "fork() failed"};
  }

  if (pid == 0) {
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    ::dup2(stdout_pipe[1], STDOUT_FILENO);
    ::dup2(stderr_pipe[1], STDERR_FILENO);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    ::execv(resolved.c_str(), argv);
    ::_exit(127);
  }

  ::close(stdout_pipe[1]);
  ::close(stderr_pipe[1]);

  std::string stdout_out = read_fd(stdout_pipe[0]);
  std::string stderr_out = read_fd(stderr_pipe[0]);

  ::close(stdout_pipe[0]);
  ::close(stderr_pipe[0]);

  // Wait with timeout
  int status = 0;
  bool timed_out = false;

  if (timeout_sec > 0) {
    // Poll waitpid with WNOHANG + sleep loop
    int elapsed = 0;
    const int poll_ms = 200;
    while (elapsed < timeout_sec * 1000) {
      int ret = ::waitpid(pid, &status, WNOHANG);
      if (ret > 0) {
        // Child exited
        break;
      }
      if (ret < 0) {
        // Error
        break;
      }
      // ret == 0: still running
      usleep(poll_ms * 1000);
      elapsed += poll_ms;
    }

    if (elapsed >= timeout_sec * 1000) {
      // Timeout: kill the child process.
      // Uses exec_safe("kill", ...) instead of raw ::kill() to avoid
      // <signal.h> dependency and comply with signal-handling prohibition.
      // Process management (killing timed-out children) is not signal handling.
      timed_out = true;
      {
        std::string pid_str = std::to_string(pid);
        exec_safe("kill", {"kill", "-15", pid_str}); // graceful stop
        usleep(100000);                              // 100ms grace period
        // Force kill if still alive
        if (::waitpid(pid, &status, WNOHANG) == 0) {
          exec_safe("kill", {"kill", "-9", pid_str}); // force kill
          ::waitpid(pid, &status, 0);
        }
      }

      std::string cmd_str = file;
      for (int i = 1; argv[i]; i++) {
        cmd_str += " ";
        cmd_str += argv[i];
      }
      stderr_out += "\n[TIMEOUT] command killed after " +
                    std::to_string(timeout_sec) + "s: " + cmd_str;
      get_logger()->error("exec_safe TIMEOUT after {}s: {}", timeout_sec,
                          cmd_str);
    }
  } else {
    // No timeout: blocking wait
    ::waitpid(pid, &status, 0);
  }

  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (timed_out) {
    exit_code = -2; // Special code for timeout
  }

  return {exit_code, stdout_out, stderr_out, timed_out};
}

ShellResult exec_safe(const std::vector<std::string> &args) {
  if (args.empty()) {
    return {-1, "", "empty argument list"};
  }
  return exec_safe(args[0], args);
}

ShellResult exec_safe(const std::string &file,
                      const std::vector<std::string> &args) {
  return exec_safe(file, args, EXEC_SAFE_TIMEOUT_DEFAULT);
}

ShellResult exec_safe(const std::string &file,
                      const std::vector<std::string> &args, int timeout_sec) {
  if (file.empty()) {
    return {-1, "", "empty command file"};
  }

  static const std::set<std::string> ALLOWED_COMMANDS = {
      "mount",   "umount",  "chmod",      "chown",    "chgrp",
      "useradd", "userdel", "groupadd",   "groupdel", "usermod",
      "passwd",  "gpasswd", "ssh-keygen", "mkdir",    "cp",
      "mv",      "getent",  "findmnt",    "which",    "ssh",
      "pkill",   "ps",      "su",         "crontab",  "kill"};
  std::string cmd_name = fs::path(file).filename().string();
  if (ALLOWED_COMMANDS.find(cmd_name) == ALLOWED_COMMANDS.end()) {
    return {-1, "", "command not in allowed list: " + cmd_name};
  }

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &a : args) {
    argv.push_back(const_cast<char *>(a.c_str()));
  }
  argv.push_back(nullptr);

  return do_fork_exec(file, argv.data(), timeout_sec);
}

bool validate_username(const std::string &username) {
  if (username.empty() || username.size() > 32)
    return false;
  if (username[0] >= '0' && username[0] <= '9')
    return false;
  for (char c : username) {
    if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' &&
        c != '-')
      return false;
  }
  return true;
}

bool validate_ssh_public_key(const std::string &key) {
  if (key.empty() || key.size() > 8192)
    return false;
  if (key.find('\'') != std::string::npos)
    return false;
  if (key.find('\n') != std::string::npos)
    return false;
  if (key.find('\r') != std::string::npos)
    return false;

  static const std::vector<std::string> valid_prefixes = {
      "ssh-ed25519 ",
      "ssh-rsa ",
      "ecdsa-sha2-nistp256 ",
      "ecdsa-sha2-nistp384 ",
      "ecdsa-sha2-nistp521 ",
      "sk-ssh-ed25519@openssh.com ",
      "sk-ecdsa-sha2-nistp256@openssh.com "};
  bool has_valid_prefix = false;
  for (const auto &p : valid_prefixes) {
    if (key.size() > p.size() && key.compare(0, p.size(), p) == 0) {
      has_valid_prefix = true;
      break;
    }
  }
  if (!has_valid_prefix)
    return false;

  std::regex re("^[a-zA-Z0-9+/=@._-]+(\\s+.+)?$");
  return std::regex_match(key, re);
}

bool validate_key_type(const std::string &key_type) {
  return key_type == "ed25519" || key_type == "rsa" || key_type == "ecdsa";
}

bool is_root() { return geteuid() == 0; }

bool command_exists(const std::string &cmd) {
  auto result = exec_safe("which", {"which", cmd});
  return result.exit_code == 0;
}

std::string get_current_username() {
  auto *pw = getpwuid(geteuid());
  return pw ? pw->pw_name : "";
}

std::string get_home_dir(const std::string &username) {
  auto *pw = getpwnam(username.c_str());
  return pw ? pw->pw_dir : "";
}

std::string get_effective_username() {
  uid_t login_uid = get_login_uid();
  if (login_uid != 0) {
    auto *pw = getpwuid(login_uid);
    if (pw && pw->pw_name)
      return pw->pw_name;
  }
  return get_current_username();
}

std::string get_effective_home() {
  if (const char *env_home = std::getenv("HOME")) {
    if (env_home[0] == '/' && env_home[1] != '\0')
      return env_home;
  }
  std::string username = get_effective_username();
  if (!username.empty()) {
    std::string home = get_home_dir(username);
    if (!home.empty())
      return home;
  }
  return get_home_dir(get_current_username());
}

bool validate_path_no_shell_metachars(const std::string &path) {
  if (path.find('\0') != std::string::npos) {
    return false;
  }
  static const std::string dangerous = ";`$(){}[]|&<>!\n\r";
  return path.find_first_of(dangerous) == std::string::npos;
}

std::string shell_escape(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

uid_t get_login_uid() {
  const char *sudo_uid_env = std::getenv("SUDO_UID");
  if (sudo_uid_env && sudo_uid_env[0] != '\0') {
    try {
      uid_t uid = std::stoul(sudo_uid_env);
      if (uid != 0)
        return uid;
    } catch (...) {
    }
  }

  std::ifstream ifs("/proc/self/loginuid");
  if (ifs) {
    uid_t uid = 0;
    ifs >> uid;
    if (uid != (uid_t)-1 && uid != 0) {
      return uid;
    }
  }
  return 0;
}

// Check if login_uid has write access to a path by examining stat permissions.
// We cannot use seteuid()+access() because when real UID is root, access()
// still uses root's DAC_OVERRIDE capability regardless of effective UID.
// Instead, we manually check owner/group/other permission bits.
static bool has_write_access(const fs::path &p, uid_t login_uid) {
  struct stat st;
  if (stat(p.c_str(), &st) != 0)
    return false;

  // 1. Owner check
  if (st.st_uid == login_uid) {
    return (st.st_mode & S_IWUSR) != 0;
  }

  // 2. Group check - get all groups for login_uid
  struct passwd *pw = getpwuid(login_uid);
  if (pw) {
    // Check primary group
    if (st.st_gid == pw->pw_gid) {
      return (st.st_mode & S_IWGRP) != 0;
    }

    // Check supplementary groups
    int ngroups = 0;
    getgrouplist(pw->pw_name, pw->pw_gid, nullptr, &ngroups);
    if (ngroups > 0) {
      std::vector<gid_t> groups(ngroups);
      if (getgrouplist(pw->pw_name, pw->pw_gid, groups.data(), &ngroups) >= 0) {
        for (int i = 0; i < ngroups; i++) {
          if (st.st_gid == groups[i]) {
            return (st.st_mode & S_IWGRP) != 0;
          }
        }
      }
    }
  }

  // 3. Other check
  return (st.st_mode & S_IWOTH) != 0;
}

// Check if a path is an ai-user home managed by the login user.
// Looks for .am_status file in the path (or its parents) and verifies
// main_user.
static bool is_managed_ai_user_home(const fs::path &p, uid_t login_uid) {
  // Check path itself and each parent for .am_status
  fs::path check = p;
  for (int i = 0; i < 10 && !check.empty() && check != "/"; ++i) {
    fs::path state_file = check / ".am_status";
    std::ifstream ifs(state_file);
    if (ifs.is_open()) {
      try {
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        // Simple JSON parse for main_user field
        auto pos = content.find("\"main_user\"");
        if (pos != std::string::npos) {
          auto val_start = content.find('"', pos + 12);
          if (val_start != std::string::npos) {
            auto val_end = content.find('"', val_start + 1);
            if (val_end != std::string::npos) {
              std::string main_user =
                  content.substr(val_start + 1, val_end - val_start - 1);
              struct passwd *pw = getpwuid(login_uid);
              if (pw && main_user == pw->pw_name) {
                return true;
              }
            }
          }
        }
      } catch (...) {
      }
      // Found .am_status but not ours — stop searching
      break;
    }
    check = check.parent_path();
  }
  return false;
}

// Helper: check if a path is under any allowed_base
static bool is_under_allowed_bases(const fs::path &p,
                                   const std::vector<fs::path> &allowed_bases) {
  if (allowed_bases.empty())
    return false;
  std::string s = p.string();
  for (const auto &base : allowed_bases) {
    std::string base_str = base.string();
    if (base_str.empty())
      continue;
    // Ensure trailing slash comparison is correct
    if (s == base_str)
      return true;
    if (s.length() > base_str.length() && s[base_str.length()] == '/' &&
        s.substr(0, base_str.length()) == base_str)
      return true;
  }
  return false;
}

bool is_path_allowed(const fs::path &p,
                     [[maybe_unused]] const std::string &main_user,
                     const std::vector<fs::path> &allowed_bases) {
  if (p.empty())
    return false;

  // Reject ".." traversal (security: prevent directory escape)
  for (const auto &part : p) {
    if (part == "..")
      return false;
  }

  // Get the real invoking user's UID (via SUDO_UID or loginuid)
  uid_t login_uid = get_login_uid();
  if (login_uid == 0)
    login_uid = geteuid();
  if (login_uid == 0) {
    // Root user (UID 0) is allowed to manage all paths
    // This is needed for Docker testing environments where tests run as root
    // SECURITY: Production environments should NOT run as root
    utils::get_logger()->debug("Root user (UID 0) operating on path: {}",
                               p.string());
    return true; // Skip all security checks for root
  }

  std::error_code ec;

  // Try canonical resolution for existing paths
  fs::path canon = fs::canonical(p, ec);
  if (!ec) {
    // Path exists: check if owned by login_uid OR user has write access
    struct stat st;
    if (stat(canon.c_str(), &st) != 0)
      return false;

    // 1. Owner check: path owned by login_uid -> allow
    if (st.st_uid == login_uid) {
      return security::validate_path_allowed(canon);
    }

    // 2. Managed ai-user home check: path is under an ai-user managed by login
    // user
    if (is_managed_ai_user_home(canon, login_uid)) {
      return security::validate_path_allowed(canon);
    }

    // 3. Permission check: user has write access via group/other
    if (has_write_access(canon, login_uid)) {
      return security::validate_path_allowed(canon);
    }

    // 4. Allowed bases: path under configured extra base paths (e.g. BeeGFS)
    //    Still requires ownership/permission on some parent, but skips
    //    SYSTEM_DIRS (allowed_bases may be under /scratch etc. which are still
    //    in SYSTEM_DIRS)
    if (is_under_allowed_bases(canon, allowed_bases)) {
      // Check parent chain for ownership or write access
      fs::path check = canon;
      while (!check.empty() && check != "/") {
        struct stat pst;
        if (stat(check.c_str(), &pst) == 0) {
          if (pst.st_uid == login_uid || has_write_access(check, login_uid)) {
            return security::validate_path_allowed_skip_system_dirs(canon);
          }
        }
        check = check.parent_path();
      }
    }

    // Path exists but no access - check parent chain for ownership
    fs::path parent = canon.parent_path();
    int max_depth = 32;
    while (!parent.empty() && parent != "/" && --max_depth > 0) {
      if (stat(parent.c_str(), &st) == 0) {
        if (st.st_uid == login_uid) {
          return security::validate_path_allowed(parent);
        }
        if (is_managed_ai_user_home(parent, login_uid)) {
          return security::validate_path_allowed(parent);
        }
        // Also check if user has write access to parent
        if (has_write_access(parent, login_uid)) {
          return security::validate_path_allowed(parent);
        }
      }
      parent = parent.parent_path();
    }

    // Path not owned by user and no access via permissions
    return false;
  }

  // Path doesn't exist: check parent permissions
  fs::path parent = p.parent_path();
  if (parent.empty())
    return false;

  fs::path canon_parent = fs::canonical(parent, ec);
  if (ec)
    return false; // Parent doesn't exist or can't resolve

  struct stat st;
  if (stat(canon_parent.c_str(), &st) != 0)
    return false;

  // 1. Parent owned by login_uid -> allow creating new path
  if (st.st_uid == login_uid) {
    return security::validate_path_allowed(canon_parent);
  }

  // 2. Parent is managed ai-user home -> allow
  if (is_managed_ai_user_home(canon_parent, login_uid)) {
    return security::validate_path_allowed(canon_parent);
  }

  // 3. User has write access to parent via group/other -> allow
  if (has_write_access(canon_parent, login_uid)) {
    return security::validate_path_allowed(canon_parent);
  }

  // 4. Allowed bases: parent under configured extra base paths
  //    Skips SYSTEM_DIRS check (allowed_bases may be under /scratch etc.)
  if (is_under_allowed_bases(canon_parent, allowed_bases)) {
    // Check parent chain for ownership or write access
    fs::path check = canon_parent;
    while (!check.empty() && check != "/") {
      struct stat pst;
      if (stat(check.c_str(), &pst) == 0) {
        if (pst.st_uid == login_uid || has_write_access(check, login_uid)) {
          return security::validate_path_allowed_skip_system_dirs(canon_parent);
        }
      }
      check = check.parent_path();
    }
  }

  // Parent not owned by user and no write access
  return false;
}

bool is_path_allowed_parent(const fs::path &p, const std::string &main_user,
                            const std::vector<fs::path> &allowed_bases) {
  fs::path parent = p.parent_path();
  if (parent.empty())
    return false;
  return is_path_allowed(parent, main_user, allowed_bases);
}

// Variant of is_path_allowed that skips SYSTEM_DIRS blacklist.
// Touch command uses this: it only needs ownership validation,
// allowing paths under /opt etc. as long as the path is owned by main_user.
bool is_path_allowed_no_system_check(
    const fs::path &p, [[maybe_unused]] const std::string &main_user,
    const std::vector<fs::path> & /*allowed_bases*/) {
  if (p.empty())
    return false;

  for (const auto &part : p) {
    if (part == "..")
      return false;
  }

  uid_t login_uid = get_login_uid();
  if (login_uid == 0)
    login_uid = geteuid();
  if (login_uid == 0) {
    utils::get_logger()->warn(
        "Cannot determine real user UID for path validation");
    return false;
  }

  std::error_code ec;
  fs::path canon = fs::canonical(p, ec);
  if (!ec) {
    // Path exists: check ownership
    struct stat st;
    if (stat(canon.c_str(), &st) != 0)
      return false;

    if (st.st_uid == login_uid)
      return true;
    if (is_managed_ai_user_home(canon, login_uid))
      return true;
    if (has_write_access(canon, login_uid))
      return true;

    // Check parent chain for ownership
    fs::path parent = canon.parent_path();
    int max_depth = 32;
    while (!parent.empty() && parent != "/" && --max_depth > 0) {
      if (stat(parent.c_str(), &st) == 0) {
        if (st.st_uid == login_uid)
          return true;
        if (is_managed_ai_user_home(parent, login_uid))
          return true;
        if (has_write_access(parent, login_uid))
          return true;
      }
      parent = parent.parent_path();
    }
    return false;
  }

  // Path doesn't exist: check parent
  fs::path parent = p.parent_path();
  if (parent.empty())
    return false;

  fs::path canon_parent = fs::canonical(parent, ec);
  if (ec)
    return false;

  struct stat st;
  if (stat(canon_parent.c_str(), &st) != 0)
    return false;

  if (st.st_uid == login_uid)
    return true;
  if (is_managed_ai_user_home(canon_parent, login_uid))
    return true;
  if (has_write_access(canon_parent, login_uid))
    return true;

  // Check parent chain
  fs::path check = canon_parent;
  int max_depth = 32;
  while (!check.empty() && check != "/" && --max_depth > 0) {
    struct stat pst;
    if (stat(check.c_str(), &pst) == 0) {
      if (pst.st_uid == login_uid)
        return true;
      if (has_write_access(check, login_uid))
        return true;
    }
    check = check.parent_path();
  }

  return false;
}

bool is_group_member(const std::string &group_name) {
  if (group_name.empty())
    return false;

  // Get the group entry
  struct group *gr = getgrnam(group_name.c_str());
  if (!gr) {
    // Group doesn't exist
    return false;
  }
  gid_t target_gid = gr->gr_gid;

  // Use get_login_uid() to get the real user (via SUDO_UID or
  // /proc/self/loginuid), falling back to geteuid() for non-sudo scenarios.
  // This ensures we check the invoking user's groups, not root's groups.
  uid_t real_uid = get_login_uid();
  if (real_uid == 0)
    real_uid = geteuid();

  struct passwd *pw = getpwuid(real_uid);
  if (!pw)
    return false;

  // Check primary group
  if (pw->pw_gid == target_gid) {
    return true;
  }

  // Check all groups for the real user using getgrouplist()
  // (not getgroups() which checks the current process's groups, i.e. root's)
  int ngroups = 0;
  getgrouplist(pw->pw_name, pw->pw_gid, nullptr, &ngroups);
  if (ngroups <= 0) {
    return false;
  }

  std::vector<gid_t> groups(ngroups);
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups.data(), &ngroups) < 0) {
    return false;
  }

  for (gid_t g : groups) {
    if (g == target_gid) {
      return true;
    }
  }

  return false;
}
// Reconcile AI user's supplementary groups against the configured groups
// from .ai-mirror.toml's [ai-user] groups field.
// See shell.hpp for full design documentation.
int reconcile_ai_user_groups(
    const std::string &ai_user, const std::string &main_user,
    const std::vector<std::string> &configured_groups) {
  auto logger = get_logger();
  int changes = 0;

  // Resolve AI user (needed for primary group)
  struct passwd *ai_pw = getpwnam(ai_user.c_str());
  if (!ai_pw) {
    logger->error("reconcile_ai_user_groups: cannot resolve AI user '{}'",
                  ai_user);
    return -1;
  }

  // ── Helper: get all group names for a user ──────────────────────────
  auto get_group_names = [](const std::string &username,
                            gid_t primary_gid) -> std::set<std::string> {
    std::set<std::string> names;

    // Include primary group
    struct group *primary_gr = getgrgid(primary_gid);
    if (primary_gr) {
      names.insert(primary_gr->gr_name);
    }

    // Get supplementary groups from system database
    struct passwd *pw = getpwnam(username.c_str());
    if (!pw)
      return names;

    int ngroups = 0;
    getgrouplist(username.c_str(), pw->pw_gid, nullptr, &ngroups);
    if (ngroups <= 0)
      return names;

    std::vector<gid_t> groups(ngroups);
    if (getgrouplist(username.c_str(), pw->pw_gid, groups.data(), &ngroups) < 0)
      return names;

    for (int i = 0; i < ngroups; i++) {
      struct group *gr = getgrgid(groups[i]);
      if (gr) {
        names.insert(gr->gr_name);
      }
    }

    return names;
  };

  // ── Get AI user's current groups ────────────────────────────────────
  std::set<std::string> ai_groups = get_group_names(ai_user, ai_pw->pw_gid);

  // Get AI user's primary group name (never remove this)
  std::string ai_primary_group;
  struct group *ai_primary_gr = getgrgid(ai_pw->pw_gid);
  if (ai_primary_gr) {
    ai_primary_group = ai_primary_gr->gr_name;
  }

  // ── Compute target groups ───────────────────────────────────────────
  // Target = configured_groups from .ai-mirror.toml plus the main user's
  // group (needed for file access), minus 'ai-mirror' (security block).
  //
  // The .ai-mirror.toml is the SOLE AUTHORITY.  We do NOT use the main
  // user's system group membership because:
  //   - The main user may have been added to AI-user-specific groups
  //     (e.g. imaxx_*) by legacy am create flow
  //   - The config explicitly lists what groups AI users should have
  std::set<std::string> target_groups;

  // Add main user's primary group (e.g. "maxx") — needed for file access
  // so main user can enter AI user's home directory
  struct passwd *main_pw = getpwnam(main_user.c_str());
  if (main_pw) {
    struct group *main_gr = getgrgid(main_pw->pw_gid);
    if (main_gr) {
      target_groups.insert(main_gr->gr_name);
    }
  }

  // Add configured groups (from .ai-mirror.toml [ai-user] groups)
  for (const auto &g : configured_groups) {
    if (g == "ai-mirror") {
      logger->warn("Config contains 'ai-mirror' in ai-user.groups — "
                   "SECURITY BLOCKED, will not add");
      continue;
    }
    target_groups.insert(g);
  }

  // ── Compute delta ───────────────────────────────────────────────────
  // Groups to add: target - current
  std::vector<std::string> to_add;
  for (const auto &g : target_groups) {
    if (ai_groups.find(g) == ai_groups.end()) {
      to_add.push_back(g);
    }
  }

  // Groups to remove: current - target - ai_primary_group
  std::vector<std::string> to_remove;
  for (const auto &g : ai_groups) {
    if (g == ai_primary_group)
      continue; // Never remove AI user's own primary group
    if (target_groups.find(g) == target_groups.end()) {
      to_remove.push_back(g);
    }
  }

  // ── Apply additions ─────────────────────────────────────────────────
  for (const auto &g : to_add) {
    auto result = exec_safe({"usermod", "-aG", g, ai_user});
    if (result.exit_code == 0) {
      logger->info("Reconciled: added '{}' to group '{}'", ai_user, g);
      changes++;
    } else {
      // [log-review] warn:降级自error——usermod 失败不影响系统正常运行，
      // 组修复将在下一轮 mount-watch 循环中重试
      logger->warn("Reconciled: failed to add '{}' to group '{}': {}", ai_user,
                   g, result.stderr_output);
    }
  }

  // ── Apply removals ──────────────────────────────────────────────────
  for (const auto &g : to_remove) {
    auto result = exec_safe({"gpasswd", "-d", ai_user, g});
    if (result.exit_code == 0) {
      logger->info("Reconciled: removed '{}' from group '{}'", ai_user, g);
      changes++;
    } else {
      // [log-review] warn:降级自error——gpasswd 失败不影响系统正常运行，
      // 组修复将在下一轮 mount-watch 循环中重试
      logger->warn("Reconciled: failed to remove '{}' from group '{}': {}",
                   ai_user, g, result.stderr_output);
    }
  }

  if (changes > 0) {
    logger->info("Reconciled groups for '{}': {} changes", ai_user, changes);
  }

  return changes;
}

} // namespace ai_mirror::utils

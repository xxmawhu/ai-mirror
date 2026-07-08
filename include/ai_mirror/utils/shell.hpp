#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::utils {

struct ShellResult {
  int exit_code;
  std::string stdout_output;
  std::string stderr_output;
  bool timed_out = false; // true if process was killed by timeout
};

// Default timeout for exec_safe commands (seconds)
static constexpr int EXEC_SAFE_TIMEOUT_DEFAULT = 30;

// Execute command with security restrictions:
// 1. Command must be in ALLOWED_COMMANDS whitelist (by basename)
// 2. Command resolved via hardcoded absolute paths (no PATH lookup)
// 3. Uses fork()+execv() instead of popen()/system()
// 4. Timeout: kills process after EXEC_SAFE_TIMEOUT_DEFAULT seconds
ShellResult exec_safe(const std::vector<std::string> &args);
ShellResult exec_safe(const std::string &file,
                      const std::vector<std::string> &args);
// With custom timeout (seconds, 0 = no timeout)
ShellResult exec_safe(const std::string &file,
                      const std::vector<std::string> &args, int timeout_sec);

// Validate username format: [a-z0-9_] only, max 32 chars, first char non-digit.
// No hyphens allowed to ensure consistency with path_resolver detection logic.
bool validate_username(const std::string &username);

// Validate SSH public key format:
// - Must start with known key type: ssh-ed25519, ssh-rsa, ecdsa-sha2-nistp256,
//   ecdsa-sha2-nistp384, ecdsa-sha2-nistp521, sk-ssh-ed25519@openssh.com,
//   sk-ecdsa-sha2-nistp256@openssh.com
// - Rejects keys with SSH options prefix (e.g., command="/bin/false"
// ssh-ed25519 ...)
// - Base64 payload validated for valid characters
bool validate_ssh_public_key(const std::string &key);

// Validate SSH key type string against known types.
bool validate_key_type(const std::string &key_type);

bool is_root();
bool command_exists(const std::string &cmd);
std::string get_current_username();
std::string get_home_dir(const std::string &username);

// Resolve the effective username using /proc/self/loginuid first,
// falls back to geteuid() lookup. Ignores SUDO_USER/PKEXEC_UID to
// prevent environment variable spoofing.
std::string get_effective_username();
std::string get_effective_home();
std::string shell_escape(const std::string &s);
bool validate_path_no_shell_metachars(const std::string &path);

// Validate path is under main_user's home directory or allowed_bases.
// Uses fs::canonical() for existing paths. For non-existent paths,
// validates parent directory exists and is canonical under home/allowed_bases.
// Rejects paths with ".." components.
// allowed_bases: extra base paths beyond $HOME (e.g. BeeGFS mount points)
bool is_path_allowed(const fs::path &p, const std::string &main_user,
                     const std::vector<fs::path> &allowed_bases = {});
bool is_path_allowed_parent(const fs::path &p, const std::string &main_user,
                            const std::vector<fs::path> &allowed_bases = {});

// Same as is_path_allowed but skips SYSTEM_DIRS blacklist check.
// Used by touch which only needs ownership validation, not path location
// restriction.
bool is_path_allowed_no_system_check(
    const fs::path &p, const std::string &main_user,
    const std::vector<fs::path> &allowed_bases = {});
uid_t get_login_uid();

// Check if the current effective user is a member of the specified group.
// Checks both primary group (from passwd entry) and supplementary groups.
bool is_group_member(const std::string &group_name);

// Reconcile AI user's supplementary groups to match the main user's groups.
//
// DESIGN:
//   The main user's group membership defines the baseline. AI users should
//   have the same group memberships as the main user, with EXCEPTIONS:
//     1. 'ai-mirror' group is NEVER added (sudoers root access — security)
//     2. AI user's own primary group is NEVER removed
//     3. Other AI users' per-project groups (matching the pattern
//        "{prefix}{main_user}_*", e.g. "imaxx_*") are NEVER added — they
//        belong to individual projects, not shared across AI users.
//
// ALGORITHM:
//   target_groups = main_user_groups - {ai-mirror, ai_primary_group,
//   ai_user_groups} to_add        = target_groups - ai_user_current_groups
//   to_remove     = ai_user_current_groups - target_groups - {ai_primary_group}
//
// PARAMS:
//   ai_user   — AI username (e.g. "imaxx_a3f2b1")
//   main_user — main username (e.g. "maxx")
//
// RETURNS:
//   Number of group changes applied (additions + removals), or -1 on error
//   (e.g. user not found).
//
// THREAD SAFETY: Not thread-safe (uses getgrnam/getpwnam which return
// pointers to static storage). Call from single-threaded context only.
int reconcile_ai_user_groups(const std::string &ai_user,
                             const std::string &main_user);

} // namespace ai_mirror::utils

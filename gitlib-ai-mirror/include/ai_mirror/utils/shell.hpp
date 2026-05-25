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
};

// Execute command with security restrictions:
// 1. Command must be in ALLOWED_COMMANDS whitelist (by basename)
// 2. Command resolved via hardcoded absolute paths (no PATH lookup)
// 3. Uses fork()+execv() instead of popen()/system()
ShellResult exec_safe(const std::vector<std::string> &args);
ShellResult exec_safe(const std::string &file,
                      const std::vector<std::string> &args);

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

} // namespace ai_mirror::utils

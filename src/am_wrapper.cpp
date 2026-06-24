// am_wrapper.cpp - am wrapper binary
// Detects if running as non-root, auto-elevates via sudo to ai-mirror-bin
//
// Architecture:
//   - /usr/local/bin/am: wrapper (this file)
//   - /usr/local/bin/ai-mirror-bin: actual implementation
//   - sudoers allows ai-mirror group to run specific ai-mirror-bin subcommands
//
// Commands requiring sudo (user/mount/chown operations):
//   create, update, rm, force-destroy, mkdir, touch, cp, mv, auto-fix-all
//
// Commands NOT requiring sudo (read-only or ssh-based):
//   cd, list, health, config, status, watch, init
//
// User calls: am <command> [args]
// Wrapper detects command needs sudo + non-root → exec sudo ai-mirror-bin
// <command> Wrapper detects command doesn't need sudo OR already root → exec
// ai-mirror-bin

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <iostream>
#include <pwd.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

static const char *REAL_BIN = "/usr/local/bin/ai-mirror-bin";

// Commands that need sudo (user management, mount, chown)
// Note: 'cd' needs sudo because it internally calls cmd_update() for auto-fix
static bool needs_sudo(const std::string &cmd) {
  static const char *sudo_cmds[] = {
      "create", "update",       "rm", "force-destroy", "mkdir", "touch", "cp",
      "mv",     "auto-fix-all", "cd", "frz",           nullptr,
  };
  for (int i = 0; sudo_cmds[i]; i++) {
    if (cmd == sudo_cmds[i])
      return true;
  }
  return false;
}

// Check if user is in ai-mirror group (from system database, not process
// groups)
static bool is_ai_mirror_group_member() {
  uid_t uid = getuid();

  // Get username from UID
  struct passwd *pw = getpwuid(uid);
  if (!pw)
    return false;

  // Get ai-mirror group GID
  struct group *gr = getgrnam("ai-mirror");
  if (!gr)
    return false;
  gid_t target_gid = gr->gr_gid;

  // Check primary group
  if (pw->pw_gid == target_gid)
    return true;

  // Get all groups for this user from system database
  int ngroups = 0;
  getgrouplist(pw->pw_name, pw->pw_gid, nullptr, &ngroups);
  if (ngroups <= 0)
    return false;

  // Use vector instead of raw new/delete (Rule 1: prefer unique_ptr/vector)
  std::vector<gid_t> groups(ngroups);
  if (getgrouplist(pw->pw_name, pw->pw_gid, groups.data(), &ngroups) < 0) {
    return false;
  }

  for (int i = 0; i < ngroups; i++) {
    if (groups[i] == target_gid) {
      return true;
    }
  }

  return false;
}

int main(int argc, char **argv) {
  if (!std::filesystem::exists(REAL_BIN)) {
    std::cerr << "error: ai-mirror binary not found: " << REAL_BIN << std::endl;
    std::cerr << "  hint: run 'bash install.sh' from ai-mirror source"
              << std::endl;
    return 1;
  }

  // Extract subcommand (skip -v/--verbose flags)
  std::string subcmd;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "-v" || arg == "--verbose")
      continue;
    if (arg.substr(0, 1) == "-")
      continue;
    subcmd = arg;
    break;
  }

  bool want_sudo = needs_sudo(subcmd);
  bool is_root = (getuid() == 0);

  // Build argv for real binary
  std::vector<char *> new_argv;

  if (want_sudo && !is_root) {
    // Non-root + needs sudo: check group membership
    if (!is_ai_mirror_group_member()) {
      std::cerr << "error: you must be a member of the 'ai-mirror' group"
                << std::endl;
      std::cerr << "  fix: sudo usermod -aG ai-mirror $USER" << std::endl;
      return 1;
    }
    // Set SUDO_UID before exec sudo, since --preserve-env=HOME prevents
    // sudo from setting SUDO_UID automatically
    // Avoid temporary object UB: store string in persistent variable
    std::string sudo_uid_str = std::to_string(getuid());
    setenv("SUDO_UID", sudo_uid_str.c_str(), 1);
    new_argv.push_back(const_cast<char *>("sudo"));
    new_argv.push_back(const_cast<char *>("--preserve-env=HOME"));
    new_argv.push_back(const_cast<char *>(REAL_BIN));
    for (int i = 1; i < argc; i++) {
      new_argv.push_back(argv[i]);
    }
    new_argv.push_back(nullptr);
    execvp("sudo", new_argv.data());
    std::cerr << "error: execvp(sudo) failed: " << strerror(errno) << std::endl;
    return 1;
  }

  // Root or no-sudo command: directly exec ai-mirror-bin
  new_argv.push_back(const_cast<char *>(REAL_BIN));
  for (int i = 1; i < argc; i++) {
    new_argv.push_back(argv[i]);
  }
  new_argv.push_back(nullptr);
  execv(REAL_BIN, new_argv.data());
  std::cerr << "error: execv failed: " << strerror(errno) << std::endl;
  return 1;
}

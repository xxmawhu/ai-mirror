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
#include <iostream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

static const char *REAL_BIN = "/usr/local/bin/ai-mirror-bin";

// Commands that need sudo (user management, mount, chown)
static bool needs_sudo(const std::string &cmd) {
  static const char *sudo_cmds[] = {
      "create", "update", "rm", "force-destroy", "mkdir",
      "touch",  "cp",     "mv", "auto-fix-all",  nullptr,
  };
  for (int i = 0; sudo_cmds[i]; i++) {
    if (cmd == sudo_cmds[i])
      return true;
  }
  return false;
}

// Check if user is in ai-mirror group
static bool is_ai_mirror_group_member() {
  gid_t groups[64];
  int ngroups = getgroups(64, groups);
  if (ngroups < 0)
    return false;

  // Get ai-mirror group GID from getent
  FILE *fp = popen("getent group ai-mirror 2>/dev/null", "r");
  if (!fp)
    return false;

  char line[256];
  gid_t gid = static_cast<gid_t>(-1);
  if (fgets(line, sizeof(line), fp)) {
    char *p = strchr(line, ':');
    if (p) {
      p = strchr(p + 1, ':');
      if (p) {
        gid = static_cast<gid_t>(atoi(p + 1));
      }
    }
  }
  pclose(fp);

  if (gid == static_cast<gid_t>(-1))
    return false;

  for (int i = 0; i < ngroups; i++) {
    if (groups[i] == gid)
      return true;
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

#include "ai_mirror/cli/commands.hpp"
#include "ai_mirror/core/config.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/daemon/health_check.hpp"
#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/daemon/watch_stats.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

// FTXUI for watch TUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <pthread.h>

namespace fs = std::filesystem;

namespace ai_mirror::cli {

// Validates that ai_user belongs to main_user. The "_" separator after
// main_user prevents prefix collision (e.g. "alice" vs "alice_bob").
// The size > check ensures at least one project-name char follows the prefix.
static bool validate_ai_user_ownership(const std::string &ai_user,
                                       const std::string &main_user,
                                       const std::string &prefix) {
  if (ai_user.empty() || main_user.empty())
    return false;
  std::string expected_prefix = prefix + main_user + "_";
  return ai_user.size() > expected_prefix.size() &&
         ai_user.substr(0, expected_prefix.size()) == expected_prefix;
}

struct CommandContext {
  core::Config config;
  std::unique_ptr<core::UserManager> user_mgr;
  std::unique_ptr<core::Graft> graft;
  std::unique_ptr<core::SSHManager> ssh_mgr;
  bool verbose = false;
};

static CommandContext make_context(bool verbose) {
  CommandContext ctx;
  ctx.config = core::ConfigParser::load_default();
  ctx.user_mgr = std::make_unique<core::UserManager>(
      ctx.config.user.prefix, ctx.config.user.allowed_bases);
  ctx.graft = std::make_unique<core::Graft>(ctx.config.user.prefix);
  ctx.ssh_mgr = std::make_unique<core::SSHManager>();
  ctx.verbose = verbose;
  return ctx;
}

// Forward declaration for use in do_configure's Third pass lambda
static bool safe_chown_path(const fs::path &p, const std::string &owner);

static int do_configure(CommandContext &ctx, const core::UserInfo &state,
                        const fs::path &proj, const std::string &main_user,
                        bool mount_only = false) {
  std::string username = state.username;
  std::string home_dir = state.home_dir;
  int fixes = 0;

  // [P1-performance] mount_only=true skips SSH/groups/permissions setup.
  // Used by cmd_auto_fix_all which should only fix mounts, not reconfigure
  // the entire project. See issue P1-auto-fix-all-performance.
  if (!mount_only) {
    {
      fs::path mhome = utils::get_home_dir(main_user);
      fs::path p = fs::absolute(proj);
      std::vector<fs::path> dirs_to_fix;
      // 防止无限循环：最多遍历到根目录或最多32层
      int max_depth = 32;
      while (p.has_parent_path() && p != mhome && !p.empty() && p != "/" &&
             --max_depth > 0) {
        p = p.parent_path();
        if (p == mhome || p == "/")
          break;
        dirs_to_fix.push_back(p);
      }
      for (auto &d : dirs_to_fix) {
        std::error_code ec;
        auto perms = fs::status(d, ec).permissions();
        if (ec)
          continue;
        if ((perms & fs::perms::group_exec) == fs::perms::none) {
          auto chgrp = utils::exec_safe({"chgrp", main_user, d.string()});
          if (chgrp.exit_code == 0) {
            fs::permissions(d, perms | fs::perms::group_exec, ec);
            if (!ec) {
              utils::get_logger()->info(
                  "Added g+x to {} (group traverse for {})", d.string(),
                  main_user);
              fixes++;
            }
          } else {
            utils::get_logger()->warn("Failed to chgrp {} to {}: {}",
                                      d.string(), main_user,
                                      chgrp.stderr_output);
          }
        }
      }
      std::error_code iter_ec;
      for (const auto &entry : fs::directory_iterator(mhome, iter_ec)) {
        if (!entry.is_directory())
          continue;
        std::error_code ec;
        auto ep = entry.status(ec).permissions();
        if (!ec && (ep & fs::perms::group_write) != fs::perms::none) {
          fs::permissions(entry.path(), ep & ~fs::perms::group_write, ec);
          if (!ec) {
            utils::get_logger()->info(
                "Removed g+w from {} (privacy protection)",
                entry.path().string());
            fixes++;
          } else {
            utils::get_logger()->warn("Failed to remove g+w from {}: {}",
                                      entry.path().string(), ec.message());
          }
        }
      }
    }

    {
      std::error_code ec;
      auto hp = fs::status(home_dir, ec);
      if (!ec && (hp.permissions() & (fs::perms::set_gid)) != fs::perms::none) {
        utils::get_logger()->info(
            "Clearing setgid on {} (will be re-applied by "
            "grant_write_access if needed)",
            home_dir);
      }
    }

    {
      fs::path passwd_home = utils::get_home_dir(main_user);
      std::string env_home = utils::get_effective_home();
      if (!passwd_home.empty() && !env_home.empty() &&
          passwd_home != env_home) {
        fs::path old_ssh = passwd_home / ".ssh";
        fs::path new_ssh = fs::path(env_home) / ".ssh";
        std::error_code ec;
        if (fs::exists(new_ssh) && !fs::exists(old_ssh, ec)) {
          fs::create_symlink(new_ssh, old_ssh, ec);
          if (!ec) {
            struct passwd *main_pw = getpwnam(main_user.c_str());
            if (!main_pw) {
              utils::get_logger()->warn("Cannot resolve uid for main user '{}'",
                                        main_user);
            } else {
              auto chown_r =
                  utils::exec_safe({"chown", "-h",
                                    std::to_string(main_pw->pw_uid) + ":" +
                                        std::to_string(main_pw->pw_gid),
                                    old_ssh.string()});
              if (chown_r.exit_code == 0) {
                utils::get_logger()->info("Created symlink {} -> {}",
                                          old_ssh.string(), new_ssh.string());
                fixes++;
              }
            }
          }
        }
      }
    }

    // Fix AM home directory permissions for collaboration
    // This allows main user to create sub-projects inside AM home
    core::UserManager::fix_home_dir_permissions(home_dir, main_user);
    fixes++; // Count as a fix even if permissions were already correct

    auto grp_result = utils::exec_safe({"usermod", "-aG", main_user, username});
    if (grp_result.exit_code == 0) {
      fixes++;
    } else {
      utils::get_logger()->warn("Failed to add {} to {} group: {}", username,
                                main_user, grp_result.stderr_output);
    }

    // Add main_user to ai_user's group for file access
    auto grp_result2 =
        utils::exec_safe({"usermod", "-aG", username, main_user});
    if (grp_result2.exit_code == 0) {
      fixes++;
      if (ctx.verbose) {
        std::cout << "newgrp=" << username << std::endl;
      }
    } else {
      utils::get_logger()->warn("Failed to add {} to {} group: {}", main_user,
                                username, grp_result2.stderr_output);
    }

    // Supplementary groups from [ai-user] config
    // Security rules:
    //   1. ai-mirror group is ALWAYS rejected (ai-user must never be in
    //   ai-mirror)
    //   2. If main_user is not a member of the requested group, reject (no
    //   privilege escalation)
    //   3. If the group does not exist on the system, warn and skip
    for (const auto &group_name : ctx.config.ai_user.groups) {
      // Rule 1: Never allow ai-mirror group
      if (group_name == "ai-mirror") {
        std::cerr << "SECURITY: refusing to add ai-user to 'ai-mirror' group"
                  << std::endl;
        continue;
      }

      // Check group exists
      struct group *grp = getgrnam(group_name.c_str());
      if (!grp) {
        std::cerr << "WARNING: group '" << group_name
                  << "' does not exist, skipping" << std::endl;
        continue;
      }

      // Rule 2: main_user must be a member of the group
      struct passwd *main_pw = getpwnam(main_user.c_str());
      if (!main_pw) {
        std::cerr << "WARNING: cannot resolve main user '" << main_user
                  << "' for group check" << std::endl;
        continue;
      }

      bool main_in_group = false;
      // Check primary group
      if (main_pw->pw_gid == grp->gr_gid) {
        main_in_group = true;
      }
      // Check supplementary groups
      if (!main_in_group) {
        int ngroups = 0;
        getgrouplist(main_user.c_str(), main_pw->pw_gid, nullptr, &ngroups);
        if (ngroups > 0) {
          std::vector<gid_t> groups(ngroups);
          if (getgrouplist(main_user.c_str(), main_pw->pw_gid, groups.data(),
                           &ngroups) >= 0) {
            for (int i = 0; i < ngroups; ++i) {
              if (groups[i] == grp->gr_gid) {
                main_in_group = true;
                break;
              }
            }
          }
        }
      }

      if (!main_in_group) {
        std::cerr << "SECURITY: refusing to add ai-user to '" << group_name
                  << "' group (main user '" << main_user << "' is not a member)"
                  << std::endl;
        continue;
      }

      // Check if ai-user is already in the group
      bool ai_in_group = false;
      struct passwd *ai_pw = getpwnam(username.c_str());
      if (ai_pw) {
        int ai_ngroups = 0;
        getgrouplist(username.c_str(), ai_pw->pw_gid, nullptr, &ai_ngroups);
        if (ai_ngroups > 0) {
          std::vector<gid_t> ai_groups(ai_ngroups);
          if (getgrouplist(username.c_str(), ai_pw->pw_gid, ai_groups.data(),
                           &ai_ngroups) >= 0) {
            for (int i = 0; i < ai_ngroups; ++i) {
              if (ai_groups[i] == grp->gr_gid) {
                ai_in_group = true;
                break;
              }
            }
          }
        }
      }

      if (ai_in_group) {
        utils::get_logger()->info("ai-user '{}' already in group '{}'",
                                  username, group_name);
        continue;
      }

      auto supp_result =
          utils::exec_safe({"usermod", "-aG", group_name, username});
      if (supp_result.exit_code == 0) {
        fixes++;
        utils::get_logger()->info(
            "Added ai-user '{}' to supplementary group '{}'", username,
            group_name);
      } else {
        std::cerr << "WARNING: failed to add ai-user to '" << group_name
                  << "' group: " << supp_result.stderr_output << std::endl;
      }
    }

    ctx.ssh_mgr->set_key_path(ctx.config.ssh.key_path);
    ctx.ssh_mgr->set_key_type(ctx.config.ssh.key_type);

    // Check if authorized_keys exists AND contains current user's public key
    fs::path auth_keys = fs::path(home_dir) / ".ssh" / "authorized_keys";
    fs::path key_pub = fs::path(ctx.config.ssh.key_path.string() + ".pub");
    bool need_ssh_fix = false;

    // Use error_code version to avoid exception on permission denied
    std::error_code ec;
    if (!fs::exists(auth_keys, ec)) {
      // Permission error means we can't check - assume need fix (root can
      // access)
      if (ec.value() == EACCES || ec.value() == EPERM) {
        utils::get_logger()->debug(
            "Cannot check {} (permission denied), assuming need fix",
            auth_keys.string());
      }
      need_ssh_fix = true;
      utils::get_logger()->info("authorized_keys missing for {}", username);
    } else {
      // Check if authorized_keys contains the key_path's public key
      bool key_found = false;
      if (fs::exists(key_pub, ec) && !ec) {
        try {
          std::ifstream pub_file(key_pub);
          std::string pub_key_line;
          if (std::getline(pub_file, pub_key_line) && !pub_key_line.empty()) {
            // Extract the base64 key body (second field) for exact matching
            auto first_space = pub_key_line.find(' ');
            auto second_space = (first_space != std::string::npos)
                                    ? pub_key_line.find(' ', first_space + 1)
                                    : std::string::npos;
            std::string key_body =
                (first_space != std::string::npos &&
                 second_space != std::string::npos)
                    ? pub_key_line.substr(first_space + 1,
                                          second_space - first_space - 1)
                    : std::string();
            if (!key_body.empty()) {
              std::ifstream auth_file(auth_keys);
              std::string auth_line;
              while (std::getline(auth_file, auth_line)) {
                if (auth_line.find(key_body) != std::string::npos) {
                  key_found = true;
                  break;
                }
              }
            }
          }
        } catch (const std::exception &e) {
          utils::get_logger()->warn("Cannot read SSH keys: {}", e.what());
        }
      }
      if (!key_found) {
        need_ssh_fix = true;
        utils::get_logger()->info(
            "authorized_keys exists but missing {}'s key, re-authorizing",
            main_user);
      }
    }

    if (need_ssh_fix) {
      utils::get_logger()->info("Fixing SSH: setup_passwordless for {}",
                                username);
      if (ctx.ssh_mgr->setup_passwordless(main_user, username)) {
        fixes++;
      } else {
        utils::get_logger()->warn("Failed to fix SSH for {}", username);
      }
    }

    if (!ctx.config.ssh.ai_default_key.empty()) {
      ctx.ssh_mgr->setup_default_key_from_file(username,
                                               ctx.config.ssh.ai_default_key);
    }

    // Sync known_hosts from main_user so AI user can SSH to known hosts
    ctx.ssh_mgr->sync_known_hosts(main_user, username);
  } // end if (!mount_only)

  ctx.graft->invalidate_cache();
  auto existing = ctx.graft->list_mounts(username);

  std::set<std::string> configured_targets;
  for (const auto &mount_path : ctx.config.mount.paths) {
    auto source_opt = core::PathResolver::resolve(mount_path.string());
    if (!source_opt)
      continue;
    fs::path source = *source_opt;
    if (!fs::exists(source))
      continue;
    fs::path target = core::PathResolver::to_ai_user_path(source, username,
                                                          main_user, home_dir);
    configured_targets.insert(target.string());
  }

  for (const auto &m : existing) {
    if (!configured_targets.count(m.target.string())) {
      utils::get_logger()->info("Cleaning stale/duplicate mount: {}",
                                m.target.string());
      auto umount_result = utils::exec_safe({"umount", m.target.string()});
      if (umount_result.exit_code != 0) {
        utils::get_logger()->warn("Failed to umount {}: {}", m.target.string(),
                                  umount_result.stderr_output);
      } else {
        fixes++;
      }
    }
  }

  int dup_cleaned = ctx.graft->cleanup_duplicate_mounts(username);
  if (dup_cleaned > 0) {
    fixes += dup_cleaned;
    utils::get_logger()->info("Cleaned {} duplicate mount(s) for {}",
                              dup_cleaned, username);
  }

  int mount_failures = 0;
  for (const auto &mount_path : ctx.config.mount.paths) {
    auto source_opt = core::PathResolver::resolve(mount_path.string());
    if (!source_opt)
      continue;
    fs::path source = *source_opt;
    if (!fs::exists(source))
      continue;
    if (!utils::is_path_allowed(source, main_user,
                                ctx.config.user.allowed_bases))
      continue;

    fs::path target = core::PathResolver::to_ai_user_path(source, username,
                                                          main_user, home_dir);

    // Check if target is mounted and whether it's stale.
    // A bind mount becomes stale when its source is deleted, making the
    // target inaccessible (stat fails). We do NOT compare inode/device:
    // on BeeGFS, bind mount targets do not preserve source inodes, so an
    // inode comparison flags every healthy mount as stale (false positive
    // that caused spurious umount + mount-point deletion — see issue
    // 2026-06-23-2026-06-23-09-10-42-810-info-Stale-mount.md).
    bool is_mounted = ctx.graft->is_mounted(target);

    bool is_stale = false;
    struct stat target_st;
    bool target_stat_ok = (stat(target.c_str(), &target_st) == 0);

    if (is_mounted && !target_stat_ok) {
      // Target inaccessible — truly stale mount (all filesystems).
      is_stale = true;
      utils::get_logger()->info(
          "Stale mount detected (target inaccessible): {}", target.string());
    }

    if (is_stale) {
      // Unmount the stale mount and prepare for remount
      auto umount_result = utils::exec_safe({"umount", "-l", target.string()});
      if (umount_result.exit_code == 0) {
        utils::get_logger()->info("Lazy unmounted stale mount: {}",
                                  target.string());
        is_mounted = false;
        // Remove the empty mount point file if it exists
        std::error_code rm_ec;
        fs::remove(target, rm_ec);
        if (!rm_ec) {
          utils::get_logger()->info("Removed empty mount point: {}",
                                    target.string());
        }
      } else {
        // [log-review] 降级为 warning: beegfs stale mount umount 可能失败
        // 但不跳过，继续尝试 bind mount 覆盖 stale mount
        utils::get_logger()->warn(
            "Stale mount umount failed, will attempt remount: {} - {}",
            target.string(), umount_result.stderr_output);
        // Keep is_mounted = true, bind_mount will attempt to overlay
        // mount --bind can override stale mounts on some filesystems
      }
    }

    if (is_mounted && !is_stale) {
      // Already mounted — fix ownership of intermediate directories only.
      // Bind mount targets are read-only (sourced from main user's home),
      // chown on them always fails with EPERM. Only the parent path chain
      // from project root needs ownership correction for AI user access.
      if (state.uid != 0 || state.gid != 0) {
        fs::path boundary = home_dir.empty()
                                ? fs::path(target.parent_path().parent_path())
                                : fs::path(home_dir);
        fs::path parent = target.parent_path();
        if (!parent.empty()) {
          fs::path p = parent;
          std::vector<fs::path> to_fix;
          while (!p.empty() && p != "/" && p != boundary) {
            struct stat st;
            if (stat(p.c_str(), &st) == 0 &&
                (st.st_uid != state.uid || st.st_gid != state.gid)) {
              to_fix.push_back(p);
            }
            p = p.parent_path();
          }
          if (!to_fix.empty()) {
            utils::get_logger()->info(
                "Fixing ownership for {} intermediate dir(s) under {}",
                to_fix.size(), parent.string());
          }
          for (auto it = to_fix.rbegin(); it != to_fix.rend(); ++it) {
            auto r = utils::exec_safe(
                {"chown",
                 std::to_string(state.uid) + ":" + std::to_string(state.gid),
                 it->string()});
            if (r.exit_code == 0) {
              utils::get_logger()->info("Fixed ownership: {} -> {}:{}",
                                        it->string(), state.uid, state.gid);
            }
          }
        }
        // NOTE: target is a read-only bind mount — skip chown.
        // chown on a read-only bind mount always fails with EPERM.
      }
      continue;
    }

    utils::get_logger()->info("Fixing mount: {} -> {}", source.string(),
                              target.string());
    if (ctx.graft->bind_mount(source, target, true, state.uid, state.gid,
                              home_dir)) {
      fixes++;
    } else {
      mount_failures++;

      // [log-review] Mount failed is an error, diagnostics use warn as debug
      // info (Rule 32: warn from error降级，用于调试诊断)
      utils::get_logger()->warn("Mount failed: {} -> {}", source.string(),
                                target.string());

      // Source path diagnostics
      struct stat source_st;
      bool source_stat_ok = (stat(source.c_str(), &source_st) == 0);
      if (source_stat_ok) {
        utils::get_logger()->warn(
            "  source stat: ino={}, dev={}, mode={}, uid={}, gid={}, size={}",
            source_st.st_ino, source_st.st_dev, source_st.st_mode,
            source_st.st_uid, source_st.st_gid, source_st.st_size);
      } else {
        utils::get_logger()->warn("  source stat failed: errno={} ({})", errno,
                                  strerror(errno));
      }

      // Target path diagnostics
      struct stat target_st;
      bool target_stat_ok = (stat(target.c_str(), &target_st) == 0);
      if (target_stat_ok) {
        utils::get_logger()->warn(
            "  target stat: ino={}, dev={}, mode={}, uid={}, gid={}, size={}",
            target_st.st_ino, target_st.st_dev, target_st.st_mode,
            target_st.st_uid, target_st.st_gid, target_st.st_size);
      } else {
        utils::get_logger()->warn("  target stat failed: errno={} ({})", errno,
                                  strerror(errno));
      }

      // Check if target is in /proc/mounts
      std::ifstream mounts("/proc/mounts");
      if (mounts.is_open()) {
        std::string line;
        bool found = false;
        while (std::getline(mounts, line)) {
          if (line.find(target.string()) != std::string::npos) {
            found = true;
            utils::get_logger()->warn("  target in /proc/mounts: {}", line);
          }
        }
        if (!found) {
          utils::get_logger()->warn("  target not found in /proc/mounts");
        }
      }

      // Filesystem type (statfs)
      struct statfs fs_info;
      if (statfs(target.c_str(), &fs_info) == 0) {
        utils::get_logger()->warn("  target fs type: {:x}, block size: {}",
                                  fs_info.f_type, fs_info.f_bsize);
      } else if (statfs(source.c_str(), &fs_info) == 0) {
        utils::get_logger()->warn("  source fs type: {:x}, block size: {}",
                                  fs_info.f_type, fs_info.f_bsize);
      }

      utils::get_logger()->warn(
          "  mount context: is_mounted={}, is_stale={}, home_dir={}",
          is_mounted, is_stale, home_dir);
    }
  }

  // Second pass: fix ownership of ALL existing mounts (including those not in
  // current config) This handles the case where mounts were created with an old
  // config and intermediate directories (e.g., .local/) were left as root:root
  if (state.uid != 0 || state.gid != 0) {
    auto all_mounts = ctx.graft->list_mounts(username);
    for (const auto &m : all_mounts) {
      fs::path boundary = fs::path(home_dir);
      fs::path parent = m.target.parent_path();
      if (!parent.empty()) {
        fs::path p = parent;
        std::vector<fs::path> to_fix;
        while (!p.empty() && p != "/" && p != boundary) {
          struct stat st;
          if (stat(p.c_str(), &st) == 0 &&
              (st.st_uid != state.uid || st.st_gid != state.gid)) {
            to_fix.push_back(p);
          }
          p = p.parent_path();
        }
        for (auto it = to_fix.rbegin(); it != to_fix.rend(); ++it) {
          auto r = utils::exec_safe(
              {"chown",
               std::to_string(state.uid) + ":" + std::to_string(state.gid),
               it->string()});
          if (r.exit_code == 0) {
            utils::get_logger()->info(
                "Fixed ownership for existing mount parent: {} -> {}:{}",
                it->string(), state.uid, state.gid);
            fixes++;
          }
        }
      }
    }
  }

  // Clean up stale entries in home_dir that are not in current mount config.
  // Handles two cases:
  //   1. Broken symlinks (lstat ok but stat fails)
  //   2. Stale bind mounts whose source no longer exists (findmnt shows
  //   //deleted)
  // Both cause tools like lsd to print "No such file or directory" errors.
  {
    // Build set of currently configured mount targets under home_dir
    std::set<std::string> configured_mount_targets;
    for (const auto &mount_path : ctx.config.mount.paths) {
      auto source_opt = core::PathResolver::resolve(mount_path.string());
      if (!source_opt)
        continue;
      fs::path target = core::PathResolver::to_ai_user_path(
          *source_opt, username, main_user, home_dir);
      configured_mount_targets.insert(target.string());
    }

    // Scan home_dir top-level entries for stale mounts/broken symlinks
    std::error_code iter_ec;
    for (const auto &entry : fs::directory_iterator(home_dir, iter_ec)) {
      if (iter_ec)
        break;
      auto ep = entry.path();
      std::string ep_str = ep.string();

      // Skip entries that are in current mount config (they are valid)
      if (configured_mount_targets.count(ep_str))
        continue;

      // Skip .am_status
      if (ep.filename() == ".am_status")
        continue;

      // Check if stat() fails but lstat() succeeds → broken symlink or stale
      // mount
      struct stat st, lst;
      bool stat_ok = (stat(ep.c_str(), &st) == 0);
      bool lstat_ok = (lstat(ep.c_str(), &lst) == 0);

      if (!stat_ok && lstat_ok) {
        // Check if it's a stale bind mount (mount point with deleted source)
        bool is_stale_mount = ctx.graft->is_mounted_live(ep);

        if (is_stale_mount) {
          // umount the stale bind mount
          utils::get_logger()->info("Lazy unmounting stale bind mount: {}",
                                    ep_str);
          auto umount_result = utils::exec_safe({"umount", "-l", ep_str});
          if (umount_result.exit_code == 0) {
            fixes++;
            // Remove the empty mount point directory/file
            std::error_code rm_ec;
            fs::remove(ep, rm_ec);
          } else {
            utils::get_logger()->warn("Failed to umount stale mount {}: {}",
                                      ep_str, umount_result.stderr_output);
          }
        } else if (S_ISLNK(lst.st_mode)) {
          // Broken symlink — just remove it
          utils::get_logger()->info("Removing broken symlink: {}", ep_str);
          std::error_code rm_ec;
          fs::remove(ep, rm_ec);
          if (!rm_ec) {
            fixes++;
          } else {
            utils::get_logger()->warn("Failed to remove broken symlink {}: {}",
                                      ep_str, rm_ec.message());
          }
        }
      }
    }
  }

  // Third pass: recursively fix ownership of ALL entries in home_dir
  // Skip .am_status (root:root by design) and bind mount targets (read-only)
  // [P1-performance] Skip in mount_only mode — ownership is not a mount
  // concern.
  if (!mount_only) {
    {
      fs::path am_status_path = fs::path(home_dir) / ".am_status";

      // Get device number of home_dir for mount boundary detection
      struct stat home_st;
      dev_t home_dev = 0;
      if (stat(home_dir.c_str(), &home_st) == 0) {
        home_dev = home_st.st_dev;
      }

      // Recursive helper: chown all entries under a directory, skipping mount
      // points Returns number of fixes applied
      std::function<int(const fs::path &, int)> recursive_chown =
          [&](const fs::path &dir_path, int depth) -> int {
        constexpr int max_depth = 3;
        if (depth > max_depth) {
          utils::get_logger()->debug("Third pass: skipping depth {} > {} at {}",
                                     depth, max_depth, dir_path.string());
          return 0;
        }
        int local_fixes = 0;
        std::error_code iter_ec;
        for (const auto &entry : fs::directory_iterator(dir_path, iter_ec)) {
          if (iter_ec)
            break;
          auto ep = entry.path();

          // Skip .am_status
          if (ep == am_status_path)
            continue;

          struct stat st;
          if (lstat(ep.c_str(), &st) != 0)
            continue;

          // Skip symlinks — only fix symlink ownership, don't recurse
          if (S_ISLNK(st.st_mode)) {
            if ((st.st_uid != state.uid || st.st_gid != state.gid)) {
              if (lchown(ep.c_str(), state.uid, state.gid) == 0) {
                utils::get_logger()->debug(
                    "Third pass: fixed symlink {} -> {}:{}", ep.string(),
                    state.uid, state.gid);
                local_fixes++;
              } else {
                // Expected for read-only bind mounts or permission issues
                utils::get_logger()->debug(
                    "Third pass: failed to fix symlink {}", ep.string());
              }
            }
            continue;
          }

          // Check if this is a mount point by comparing device numbers
          // Bind mounts on same filesystem have same dev, so also check
          // is_mounted()
          bool is_mount_point = false;
          if (S_ISDIR(st.st_mode)) {
            // Device number changed = crossed a mount boundary
            if (home_dev != 0 && st.st_dev != home_dev) {
              is_mount_point = true;
            }
            // Also check via mount table (catches same-filesystem bind mounts)
            if (!is_mount_point && ctx.graft->is_mounted_live(ep)) {
              is_mount_point = true;
            }
          } else if (S_ISREG(st.st_mode)) {
            // Regular files can also be bind-mounted (e.g. dotfile mounts on
            // beegfs)
            if (ctx.graft->is_mounted_live(ep)) {
              is_mount_point = true;
            }
          }

          if (is_mount_point) {
            // Don't chown or recurse into mount points (read-only bind mounts)
            continue;
          }

          bool needs_chown = (st.st_uid != state.uid || st.st_gid != state.gid);
          if (needs_chown) {
            int fd = open(ep.c_str(),
                          O_RDONLY | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0) |
                              O_NOFOLLOW);
            if (fd >= 0) {
              if (fchown(fd, state.uid, state.gid) == 0) {
                utils::get_logger()->debug("Third pass: fixed {} -> {}:{}",
                                           ep.string(), state.uid, state.gid);
                local_fixes++;
              } else {
                // Expected for read-only bind mounts; these are caught by mount
                // detection above
                utils::get_logger()->debug("Third pass: failed to fix {}",
                                           ep.string());
              }
              close(fd);
            } else if (errno == ELOOP) {
              if (lchown(ep.c_str(), state.uid, state.gid) == 0) {
                utils::get_logger()->debug(
                    "Third pass: fixed symlink {} -> {}:{}", ep.string(),
                    state.uid, state.gid);
                local_fixes++;
              }
            }
          }

          // Recurse into sub-directories (non-mount, non-symlink)
          if (S_ISDIR(st.st_mode)) {
            // .git directories need full recursive chown regardless of depth
            // limit because git internals (objects/pack/, refs/heads/feature/)
            // can be 4-5+ levels deep, and incorrect ownership causes
            // Permission denied on git ops.
            if (ep.filename() == ".git") {
              if (safe_chown_path(ep, username)) {
                utils::get_logger()->debug("Third pass: deep chown .git -> {}",
                                           username);
                local_fixes++;
              } else {
                // error: chown .git failed, git operations may fail with
                // Permission denied downgrade to warn because ai-user can still
                // function, only git ops affected
                utils::get_logger()->warn(
                    "Third pass: deep chown .git failed (git ops may fail): {}",
                    ep.string());
              }
            } else {
              local_fixes += recursive_chown(ep, depth + 1);
            }
          }
        }
        return local_fixes;
      };

      int chown_fixes = recursive_chown(fs::path(home_dir), 0);
      if (chown_fixes > 0) {
        utils::get_logger()->info("Third pass: fixed {} entries in {}",
                                  chown_fixes, home_dir);
      }
      fixes += chown_fixes;
    }
  } // end if (!mount_only) — skip third pass recursive chown

  if (mount_failures > 0) {
    utils::get_logger()->warn("do_configure completed with {} mount failure(s)",
                              mount_failures);
  }

  if (!ctx.graft->grant_write_access(proj, username)) {
    utils::get_logger()->warn("Failed to grant write access to: {}",
                              proj.string());
  }

  // [P1-performance] Skip SSH StrictModes in mount_only mode.
  if (!mount_only) {
    // Fix SSH StrictModes compatibility: sshd requires the user's home
    // directory and ~/.ssh to NOT be group-writable. Since home_dir == proj and
    // we just set it to 775 (g+rwx) via grant_write_access, we must:
    //   1. Remove g+w from home_dir to satisfy sshd StrictModes
    //   2. Ensure .ssh directory is 700
    //   3. Ensure .ssh/authorized_keys is 600
    // The main user can still access files via group membership + setgid on
    // subdirectories created after newgrp.
    {
      std::error_code ec;
      auto hp = fs::status(home_dir, ec);
      if (!ec && (hp.permissions() & fs::perms::set_gid) != fs::perms::none) {
        fs::permissions(home_dir, hp.permissions() & ~fs::perms::set_gid, ec);
        if (!ec) {
          utils::get_logger()->info("Cleared setgid on home_dir {}", home_dir);
        }
      }
    }

    {
      // Remove g+w from home_dir for sshd StrictModes compatibility
      std::error_code ec;
      auto hp = fs::status(home_dir, ec);
      if (!ec &&
          (hp.permissions() & fs::perms::group_write) != fs::perms::none) {
        fs::permissions(home_dir, hp.permissions() & ~fs::perms::group_write,
                        ec);
        if (!ec) {
          utils::get_logger()->info(
              "Removed g+w from home_dir {} (sshd StrictModes compatibility)",
              home_dir);
          fixes++;
        }
      }
    }

    {
      // Ensure .ssh is 700 and authorized_keys is 600, owned by ai-user
      // (sshd StrictModes requires correct ownership)
      fs::path ssh_dir = fs::path(home_dir) / ".ssh";
      fs::path auth_keys = ssh_dir / "authorized_keys";
      std::error_code ec;
      struct passwd *ai_pw = getpwnam(username.c_str());

      if (fs::exists(ssh_dir, ec) && !ec) {
        // Fix ownership
        if (ai_pw) {
          int ssh_fd =
              open(ssh_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
          if (ssh_fd >= 0) {
            struct stat st;
            if (fstat(ssh_fd, &st) == 0 &&
                (st.st_uid != ai_pw->pw_uid || st.st_gid != ai_pw->pw_gid)) {
              if (fchown(ssh_fd, ai_pw->pw_uid, ai_pw->pw_gid) == 0) {
                utils::get_logger()->info(
                    "Fixed .ssh ownership to {} (was uid={})", username,
                    st.st_uid);
                fixes++;
              }
            }
            close(ssh_fd);
          }
        }
        // Fix permissions
        int ssh_fd = open(ssh_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (ssh_fd >= 0) {
          if (fchmod(ssh_fd, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
            utils::get_logger()->warn("Failed to chmod .ssh to 700: {}",
                                      strerror(errno));
          } else {
            utils::get_logger()->info(
                "Set .ssh to 700 for StrictModes compatibility");
            fixes++;
          }
          close(ssh_fd);
        }
      }

      if (fs::exists(auth_keys, ec) && !ec) {
        // Fix ownership
        if (ai_pw) {
          int ak_fd = open(auth_keys.c_str(), O_RDONLY | O_NOFOLLOW);
          if (ak_fd >= 0) {
            struct stat st;
            if (fstat(ak_fd, &st) == 0 &&
                (st.st_uid != ai_pw->pw_uid || st.st_gid != ai_pw->pw_gid)) {
              if (fchown(ak_fd, ai_pw->pw_uid, ai_pw->pw_gid) == 0) {
                utils::get_logger()->info(
                    "Fixed authorized_keys ownership to {} (was uid={})",
                    username, st.st_uid);
                fixes++;
              }
            }
            close(ak_fd);
          }
        }
        // Fix permissions
        int ak_fd = open(auth_keys.c_str(), O_RDONLY | O_NOFOLLOW);
        if (ak_fd >= 0) {
          if (fchmod(ak_fd, S_IRUSR | S_IWUSR) != 0) {
            utils::get_logger()->warn(
                "Failed to chmod authorized_keys to 600: {}", strerror(errno));
          } else {
            utils::get_logger()->info(
                "Set authorized_keys to 600 for StrictModes compatibility");
            fixes++;
          }
          close(ak_fd);
        }
      }
    }
  } // end if (!mount_only) — skip SSH StrictModes

  // Persist mount info to .am_status so mount_watch can read it
  if (!core::UserManager::update_state_mounts(username, home_dir,
                                              ctx.config.user.prefix)) {
    utils::get_logger()->warn(
        "Failed to update mount state in .am_status for {}", username);
  }

  utils::get_logger()->info("Configure complete: {} fix(es) applied for {}",
                            fixes, username);
  return mount_failures > 0 ? 1 : 0;
}

int cmd_create(const std::string &project_path, bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror create requires root privileges" << std::endl;
    return 1;
  }

  auto proj_opt = core::PathResolver::resolve(project_path);
  if (!proj_opt) {
    std::cerr << "Invalid project path: " << project_path << std::endl;
    return 1;
  }
  fs::path proj = *proj_opt;

  std::string main_user = utils::get_effective_username();
  if (!utils::is_path_allowed(proj, main_user, ctx.config.user.allowed_bases)) {
    std::cerr << "Path not allowed: " << proj.string() << std::endl;
    return 1;
  }

  // Suppress verbose info-level messages for concise step/pass output
  auto logger = utils::get_logger();
  auto saved_level = logger->level();
  if (!verbose) {
    logger->flush();
    logger->set_level(spdlog::level::warn);
  }

  // Step 1: Create AI user
  auto user_info = ctx.user_mgr->create_ai_user(proj.string());
  if (!user_info.exists) {
    if (!verbose) {
      logger->flush();
      logger->set_level(saved_level);
    }
    std::cout << "❌ Create AI user: failed - " << user_info.error << std::endl;
    return 1;
  }
  std::cout << "✅ Create AI user: pass" << std::endl;

  // Step 2: Configure project (SSH, mounts, permissions, write access)
  int rc = do_configure(ctx, user_info, proj, main_user);

  if (!verbose) {
    logger->flush();
    logger->set_level(saved_level);
  }

  if (rc != 0) {
    std::cout << "❌ Configure project: failed" << std::endl;
  } else {
    std::cout << "✅ Configure project: pass" << std::endl;
  }

  std::cout << user_info.username << std::endl;
  return rc;
}

static bool safe_chown_file(const fs::path &p, const std::string &owner) {
  int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    utils::get_logger()->error("safe_chown_file: open({}) failed: {}",
                               p.string(), strerror(errno));
    return false;
  }
  struct passwd *pw = getpwnam(owner.c_str());
  if (!pw) {
    close(fd);
    utils::get_logger()->error("safe_chown_file: user '{}' not found", owner);
    return false;
  }
  int ret = fchown(fd, pw->pw_uid, pw->pw_gid);
  close(fd);
  if (ret != 0) {
    utils::get_logger()->error("safe_chown_file: fchown({}) failed: {}",
                               p.string(), strerror(errno));
    return false;
  }
  return true;
}

static bool safe_chown_single(const fs::path &p, uid_t uid, gid_t gid) {
  int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ELOOP) {
      if (lchown(p.c_str(), uid, gid) != 0) {
        utils::get_logger()->error("safe_chown: lchown({}) failed: {}",
                                   p.string(), strerror(errno));
        return false;
      }
      return true;
    }
    utils::get_logger()->error("safe_chown: open({}) failed: {}", p.string(),
                               strerror(errno));
    return false;
  }
  int ret = fchown(fd, uid, gid);
  close(fd);
  if (ret != 0) {
    utils::get_logger()->error("safe_chown: fchown({}) failed: {}", p.string(),
                               strerror(errno));
    return false;
  }
  return true;
}

// FD-based recursive chown using openat()+fchownat() with O_NOFOLLOW.
// Opens each directory by fd to prevent TOCTOU symlink injection during
// traversal: an attacker cannot inject a symlink that redirects the walk
// outside the intended subtree because every component is opened relative
// to its parent directory fd with O_NOFOLLOW (symlinks fail with ELOOP).
// Symlinks at the leaf are handled with lchown (change the link itself).
static bool chown_recursive_fd(int dirfd, uid_t uid, gid_t gid, int depth = 0) {
  constexpr int max_depth = 1000;
  if (depth >= max_depth) {
    utils::get_logger()->error("chown_recursive_fd: max depth {} exceeded",
                               max_depth);
    return false;
  }

  DIR *d = fdopendir(dirfd);
  if (!d) {
    utils::get_logger()->error("safe_chown_path: fdopendir failed: {}",
                               strerror(errno));
    close(dirfd);
    return false;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != nullptr) {
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    int fd = -1;
    int retries = 0;
    while (retries < 3) {
      fd = openat(dirfd, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd >= 0 || errno != EINTR)
        break;
      retries++;
    }
    if (fd < 0) {
      if (errno == ELOOP) {
        if (fchownat(dirfd, entry->d_name, uid, gid, AT_SYMLINK_NOFOLLOW) !=
            0) {
          utils::get_logger()->warn("safe_chown_path: lchown {} failed: {}",
                                    entry->d_name, strerror(errno));
        }
        continue;
      }
      utils::get_logger()->warn("safe_chown_path: openat {} failed: {}",
                                entry->d_name, strerror(errno));
      continue;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
      close(fd);
      utils::get_logger()->warn("safe_chown_path: fstat {} failed: {}",
                                entry->d_name, strerror(errno));
      continue;
    }

    bool needs_chown = (st.st_uid != uid || st.st_gid != gid);
    if (S_ISDIR(st.st_mode)) {
      // chown the directory itself before recursing into it
      if (needs_chown) {
        if (fchown(fd, uid, gid) != 0) {
          utils::get_logger()->warn("safe_chown_path: fchown dir {} failed: {}",
                                    entry->d_name, strerror(errno));
        }
      }
      if (!chown_recursive_fd(fd, uid, gid, depth + 1)) {
        closedir(d);
        return false;
      }
    } else {
      if (needs_chown) {
        if (fchown(fd, uid, gid) != 0) {
          utils::get_logger()->warn("safe_chown_path: fchown {} failed: {}",
                                    entry->d_name, strerror(errno));
        }
      }
      close(fd);
    }
  }

  closedir(d);
  return true;
}

// FD-based recursive chmod to strip setuid/setgid bits without following
// symlinks. Uses fchmodat(AT_SYMLINK_NOFOLLOW) so symlinks are chmod'ed
// directly, preventing symlink traversal attacks.  Regular files/dirs use
// fchmod.
static bool chmod_recursive_fd(int dirfd, mode_t clear_bits) {
  DIR *d = fdopendir(dirfd);
  if (!d) {
    utils::get_logger()->error("chmod_recursive_fd: fdopendir failed: {}",
                               strerror(errno));
    close(dirfd);
    return false;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != nullptr) {
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
      continue;
    }

    int fd = -1;
    int retries = 0;
    while (retries < 3) {
      fd = openat(dirfd, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd >= 0 || errno != EINTR)
        break;
      retries++;
    }
    if (fd < 0) {
      if (errno == ELOOP) {
        mode_t link_mode = 0777;
        struct stat link_st;
        if (fstatat(dirfd, entry->d_name, &link_st, AT_SYMLINK_NOFOLLOW) == 0) {
          link_mode = link_st.st_mode;
        }
        mode_t new_mode = link_mode & ~clear_bits;
        if (fchmodat(dirfd, entry->d_name, new_mode, AT_SYMLINK_NOFOLLOW) !=
            0) {
          utils::get_logger()->warn(
              "chmod_recursive_fd: fchmodat symlink {} failed: {}",
              entry->d_name, strerror(errno));
        }
        continue;
      }
      if (errno == ENOTDIR || errno == ENOENT) {
        continue;
      }
      utils::get_logger()->warn("chmod_recursive_fd: openat {} failed: {}",
                                entry->d_name, strerror(errno));
      continue;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
      close(fd);
      utils::get_logger()->warn("chmod_recursive_fd: fstat {} failed: {}",
                                entry->d_name, strerror(errno));
      continue;
    }

    mode_t new_mode = st.st_mode & ~clear_bits;
    if (S_ISDIR(st.st_mode)) {
      if (fchmod(fd, new_mode) != 0) {
        utils::get_logger()->warn(
            "chmod_recursive_fd: fchmod dir {} failed: {}", entry->d_name,
            strerror(errno));
      }
      if (!chmod_recursive_fd(fd, clear_bits)) {
        closedir(d);
        return false;
      }
    } else {
      if (fchmod(fd, new_mode) != 0) {
        utils::get_logger()->warn("chmod_recursive_fd: fchmod {} failed: {}",
                                  entry->d_name, strerror(errno));
      }
      close(fd);
    }
  }

  closedir(d);
  return true;
}

// Recursively changes ownership of a path using fd-based traversal.
// Uses O_NOFOLLOW at every level to detect and skip symlinks, preventing
// TOCTOU attacks where an attacker replaces a directory entry with a symlink
// pointing outside the subtree between readdir() and chown().  The root
// directory is also opened with O_NOFOLLOW so the entry point itself cannot
// be a symlink.  After ownership transfer, setuid/setgid bits are stripped.
static bool safe_chown_path(const fs::path &p, const std::string &owner) {
  struct passwd *pw = getpwnam(owner.c_str());
  if (!pw) {
    utils::get_logger()->error("safe_chown_path: user '{}' not found", owner);
    return false;
  }

  int rootfd = open(p.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (rootfd < 0) {
    if (errno == ENOTDIR || errno == ELOOP) {
      return safe_chown_single(p, pw->pw_uid, pw->pw_gid);
    }
    utils::get_logger()->error("safe_chown_path: open({}) failed: {}",
                               p.string(), strerror(errno));
    return false;
  }

  if (!chown_recursive_fd(rootfd, pw->pw_uid, pw->pw_gid)) {
    return false;
  }

  int topfd = open(p.c_str(), O_RDONLY | O_DIRECTORY);
  if (topfd >= 0) {
    if (fchown(topfd, pw->pw_uid, pw->pw_gid) != 0) {
      utils::get_logger()->error("safe_chown_path: fchown root {} failed: {}",
                                 p.string(), strerror(errno));
      close(topfd);
      return false;
    }
    close(topfd);
  }

  int chmodfd = open(p.c_str(), O_RDONLY | O_DIRECTORY);
  if (chmodfd >= 0) {
    mode_t clear_bits = S_ISUID | S_ISGID;
    struct stat root_st;
    if (fstat(chmodfd, &root_st) == 0) {
      mode_t new_root_mode = root_st.st_mode & ~clear_bits;
      if (fchmod(chmodfd, new_root_mode) != 0) {
        utils::get_logger()->warn("safe_chown_path: fchmod root {} failed: {}",
                                  p.string(), strerror(errno));
      }
    }
    if (!chmod_recursive_fd(chmodfd, clear_bits)) {
      utils::get_logger()->warn(
          "safe_chown_path: chmod_recursive_fd failed for {}", p.string());
    }
  }
  return true;
}

int cmd_mkdir(const std::string &path, const std::string &ai_user,
              bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror mkdir requires root privileges" << std::endl;
    return 1;
  }

  if (!utils::validate_username(ai_user)) {
    std::cerr << "Invalid ai_user name: " << ai_user << std::endl;
    return 1;
  }

  std::string main_user = utils::get_effective_username();
  if (!validate_ai_user_ownership(ai_user, main_user, ctx.config.user.prefix)) {
    std::cerr << "ai_user '" << ai_user << "' does not belong to user '"
              << main_user << "'" << std::endl;
    return 1;
  }

  auto dir_path_opt = core::PathResolver::resolve(path);
  if (!dir_path_opt) {
    std::cerr << "Invalid path: " << path << std::endl;
    return 1;
  }
  fs::path dir_path = *dir_path_opt;

  if (!utils::is_path_allowed(dir_path, main_user,
                              ctx.config.user.allowed_bases)) {
    std::cerr << "Path not allowed: " << dir_path.string() << std::endl;
    return 1;
  }

  std::error_code ec;
  if (!fs::exists(dir_path, ec)) {
    if (!security::safe_create_directories(dir_path)) {
      std::cerr << "Failed to create directory: " << dir_path.string()
                << std::endl;
      return 1;
    }
  }

  if (!ctx.graft->grant_write_access(dir_path, ai_user)) {
    std::cerr << "Failed to grant write access" << std::endl;
    return 1;
  }

  if (verbose) {
    std::cout << "Granted write access: " << dir_path.string() << " -> "
              << ai_user << std::endl;
  }
  return 0;
}

int cmd_touch(const std::string &path, const std::string &ai_user,
              bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror touch requires root privileges" << std::endl;
    return 1;
  }

  if (!utils::validate_username(ai_user)) {
    std::cerr << "Invalid ai_user name: " << ai_user << std::endl;
    return 1;
  }

  std::string main_user = utils::get_effective_username();
  if (!validate_ai_user_ownership(ai_user, main_user, ctx.config.user.prefix)) {
    std::cerr << "ai_user '" << ai_user << "' does not belong to user '"
              << main_user << "'" << std::endl;
    return 1;
  }

  auto file_path_opt = core::PathResolver::resolve(path);
  if (!file_path_opt) {
    std::cerr << "Invalid path: " << path << std::endl;
    return 1;
  }
  fs::path file_path = *file_path_opt;

  if (!utils::is_path_allowed_no_system_check(file_path, main_user,
                                              ctx.config.user.allowed_bases)) {
    std::cerr << "Path not allowed: " << file_path.string() << std::endl;
    return 1;
  }

  // If path is an existing directory, recursively chown all contents
  std::error_code dir_ec;
  if (fs::is_directory(file_path, dir_ec) && !dir_ec) {
    if (!safe_chown_path(file_path, ai_user)) {
      std::cerr << "Failed to recursively set ownership for " << ai_user
                << std::endl;
      return 1;
    }
    if (verbose) {
      std::cout << "Recursively set ownership: " << file_path.string()
                << " (owner: " << ai_user << ")" << std::endl;
    }
    return 0;
  }

  // Single file: create if not exists, then chown
  if (!fs::exists(file_path)) {
    std::error_code ec;
    fs::path parent = file_path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
      if (!utils::is_path_allowed_no_system_check(
              parent, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Parent path not allowed: " << parent.string()
                  << std::endl;
        return 1;
      }
      if (!security::safe_create_directories(parent)) {
        std::cerr << "Failed to create parent directory: " << parent.string()
                  << std::endl;
        return 1;
      }
    }
    int fd =
        open(file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
      std::cerr << "Failed to create file: " << file_path.string() << std::endl;
      return 1;
    }
    close(fd);
  }

  if (!safe_chown_file(file_path, ai_user)) {
    std::cerr << "Failed to set ownership for " << ai_user << std::endl;
    return 1;
  }

  if (verbose) {
    std::cout << "Created file: " << file_path.string()
              << " (owner: " << ai_user << ")" << std::endl;
  }
  return 0;
}

int cmd_frz(const std::string &file_path, bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror frz requires root privileges" << std::endl;
    return 1;
  }

  auto resolved_opt = core::PathResolver::resolve(file_path);
  if (!resolved_opt) {
    std::cerr << "Invalid path: " << file_path << std::endl;
    return 1;
  }
  fs::path file = *resolved_opt;

  std::string main_user = utils::get_effective_username();

  // Check path is allowed
  if (!utils::is_path_allowed(file, main_user, ctx.config.user.allowed_bases)) {
    std::cerr << "Path not allowed: " << file.string() << std::endl;
    return 1;
  }

  // Check file exists
  std::error_code ec;
  if (!fs::exists(file, ec)) {
    std::cerr << "File does not exist: " << file.string() << std::endl;
    return 1;
  }

  // Must be a regular file (not symlink, not directory)
  struct stat st;
  if (lstat(file.c_str(), &st) != 0) {
    std::cerr << "Cannot stat file: " << file.string() << std::endl;
    return 1;
  }
  if (S_ISLNK(st.st_mode)) {
    std::cerr << "Cannot freeze a symlink: " << file.string() << std::endl;
    return 1;
  }
  if (S_ISDIR(st.st_mode)) {
    std::cerr << "Cannot freeze a directory: " << file.string() << std::endl;
    return 1;
  }
  if (!S_ISREG(st.st_mode)) {
    std::cerr << "Not a regular file: " << file.string() << std::endl;
    return 1;
  }

  // Detect file owner
  std::string owner = core::PathResolver::detect_owner_user(file);
  if (owner.empty()) {
    std::cerr << "Cannot determine file owner: " << file.string() << std::endl;
    return 1;
  }

  // Owner must be an ai-user belonging to main_user
  if (!validate_ai_user_ownership(owner, main_user, ctx.config.user.prefix)) {
    std::cerr << "File owner '" << owner << "' is not an ai-user of '"
              << main_user << "'" << std::endl;
    return 1;
  }

  // Resolve main user's uid/gid
  struct passwd *main_pw = getpwnam(main_user.c_str());
  if (!main_pw) {
    std::cerr << "Cannot resolve main user: " << main_user << std::endl;
    return 1;
  }

  // Open with O_NOFOLLOW for TOCTOU safety (re-stat after lstat validation)
  int fd = open(file.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ELOOP) {
      std::cerr << "File replaced with symlink: " << file.string() << std::endl;
    } else {
      std::cerr << "Cannot open file: " << file.string() << " - "
                << strerror(errno) << std::endl;
    }
    return 1;
  }

  // Double-check: verify it's still a regular file (TOCTOU guard)
  struct stat fd_st;
  if (fstat(fd, &fd_st) != 0 || !S_ISREG(fd_st.st_mode)) {
    close(fd);
    std::cerr << "File is no longer a regular file: " << file.string()
              << std::endl;
    return 1;
  }

  // chown to main_user:main_user_group
  if (fchown(fd, main_pw->pw_uid, main_pw->pw_gid) != 0) {
    close(fd);
    std::cerr << "Failed to change owner to " << main_user << ": "
              << strerror(errno) << std::endl;
    return 1;
  }

  // chmod to 644 (rw-r--r--)
  if (fchmod(fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0) {
    close(fd);
    std::cerr << "Failed to set permissions to 644: " << strerror(errno)
              << std::endl;
    return 1;
  }

  close(fd);

  if (verbose) {
    std::cout << "❄️ Froze: " << file.string() << " (" << owner << " -> "
              << main_user << ", 644)" << std::endl;
  } else {
    std::cout << "❄️" << std::endl;
  }
  return 0;
}

int cmd_cp(const std::string &src, const std::string &dst, bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror cp requires root privileges" << std::endl;
    return 1;
  }

  std::string main_user = utils::get_effective_username();

  auto src_path_opt = core::PathResolver::resolve(src);
  if (!src_path_opt || !fs::exists(*src_path_opt)) {
    std::cerr << "Source does not exist: " << src << std::endl;
    return 1;
  }
  fs::path src_path = *src_path_opt;

  auto dst_path_opt = core::PathResolver::resolve(dst);
  if (!dst_path_opt) {
    std::cerr << "Invalid destination path: " << dst << std::endl;
    return 1;
  }
  fs::path dst_path = *dst_path_opt;

  if (!utils::is_path_allowed(dst_path, main_user,
                              ctx.config.user.allowed_bases)) {
    std::cerr << "Destination path not allowed: " << dst_path.string()
              << std::endl;
    return 1;
  }

  if (!utils::is_path_allowed(src_path, main_user,
                              ctx.config.user.allowed_bases)) {
    std::cerr << "Source path not allowed: " << src_path.string() << std::endl;
    return 1;
  }

  // --- Dual ai-user detection ---
  // Detect ai-user from source path (file itself and its parent directory)
  std::string src_ai_user = core::PathResolver::detect_ai_user_from_path(
      src_path, main_user, ctx.config.user.prefix);
  if (src_ai_user.empty()) {
    src_ai_user = core::PathResolver::detect_ai_user_from_path(
        src_path.parent_path(), main_user, ctx.config.user.prefix);
  }
  if (src_ai_user.empty()) {
    std::string src_owner = core::PathResolver::detect_owner_user(src_path);
    if (!src_owner.empty() &&
        validate_ai_user_ownership(src_owner, main_user,
                                   ctx.config.user.prefix)) {
      src_ai_user = src_owner;
    }
  }

  // Detect ai-user from destination path (path components first, then
  // stat-based)
  std::string dst_ai_user = core::PathResolver::detect_ai_user_from_path(
      dst_path, main_user, ctx.config.user.prefix);
  if (dst_ai_user.empty() && fs::is_directory(dst_path)) {
    std::string dir_owner = core::PathResolver::detect_owner_user(dst_path);
    if (!dir_owner.empty() &&
        validate_ai_user_ownership(dir_owner, main_user,
                                   ctx.config.user.prefix)) {
      dst_ai_user = dir_owner;
    }
  }

  // --- Source ai-user security check ---
  if (!src_ai_user.empty()) {
    if (!validate_ai_user_ownership(src_ai_user, main_user,
                                    ctx.config.user.prefix)) {
      std::cerr << "Source belongs to ai-user '" << src_ai_user
                << "' which does not belong to user '" << main_user << "'"
                << std::endl;
      return 1;
    }
  }

  // --- Destination ai-user security check ---
  if (!dst_ai_user.empty()) {
    if (!validate_ai_user_ownership(dst_ai_user, main_user,
                                    ctx.config.user.prefix)) {
      std::cerr << "Destination ai-user '" << dst_ai_user
                << "' does not belong to user '" << main_user << "'"
                << std::endl;
      return 1;
    }
  }

  // --- Determine chown target ---
  // Same logic as cmd_mv: A=dst_ai, B=dst_ai, C=main_user, D=rejected, E=none
  bool need_chown = false;
  std::string chown_user;

  if (!dst_ai_user.empty()) {
    need_chown = true;
    chown_user = dst_ai_user;
  } else if (!src_ai_user.empty()) {
    need_chown = true;
    chown_user = main_user;
  }

  mode_t old_umask = umask(0077);
  auto cp_result = utils::exec_safe({"cp", "-rP", "--no-preserve=mode",
                                     src_path.string(), dst_path.string()});
  if (cp_result.exit_code != 0) {
    umask(old_umask);
    std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
    return 1;
  }

  if (need_chown) {
    fs::path chown_target =
        fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
    if (!safe_chown_path(chown_target, chown_user)) {
      umask(old_umask);
      std::cerr << "Failed to set ownership for " << chown_user << std::endl;
      return 1;
    }
  }
  umask(old_umask);

  if (verbose) {
    if (need_chown) {
      std::cout << "Copied: " << src_path.string() << " -> "
                << dst_path.string() << " (owner: " << chown_user << ")"
                << std::endl;
    } else {
      std::cout << "Copied: " << src_path.string() << " -> "
                << dst_path.string() << std::endl;
    }
  }
  return 0;
}

int cmd_mv(const std::string &src, const std::string &dst, bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror mv requires root privileges" << std::endl;
    return 1;
  }

  std::string main_user = utils::get_effective_username();

  auto src_path_opt = core::PathResolver::resolve(src);
  if (!src_path_opt || !fs::exists(*src_path_opt)) {
    std::cerr << "Source does not exist: " << src << std::endl;
    return 1;
  }
  fs::path src_path = *src_path_opt;

  auto dst_path_opt = core::PathResolver::resolve(dst);
  if (!dst_path_opt) {
    std::cerr << "Invalid destination path: " << dst << std::endl;
    return 1;
  }
  fs::path dst_path = *dst_path_opt;

  if (!utils::is_path_allowed(dst_path, main_user,
                              ctx.config.user.allowed_bases)) {
    std::cerr << "Destination path not allowed: " << dst_path.string()
              << std::endl;
    return 1;
  }

  if (!utils::is_path_allowed(src_path, main_user,
                              ctx.config.user.allowed_bases)) {
    std::cerr << "Source path not allowed: " << src_path.string() << std::endl;
    return 1;
  }

  // --- Dual ai-user detection ---
  // Detect ai-user from source path (file itself and its parent directory)
  std::string src_ai_user = core::PathResolver::detect_ai_user_from_path(
      src_path, main_user, ctx.config.user.prefix);
  if (src_ai_user.empty()) {
    src_ai_user = core::PathResolver::detect_ai_user_from_path(
        src_path.parent_path(), main_user, ctx.config.user.prefix);
  }
  // If still empty, try stat-based owner detection
  if (src_ai_user.empty()) {
    std::string src_owner = core::PathResolver::detect_owner_user(src_path);
    if (!src_owner.empty() &&
        validate_ai_user_ownership(src_owner, main_user,
                                   ctx.config.user.prefix)) {
      src_ai_user = src_owner;
    }
  }

  // Detect ai-user from destination path (path components first)
  std::string dst_ai_user = core::PathResolver::detect_ai_user_from_path(
      dst_path, main_user, ctx.config.user.prefix);
  // If dst is a directory, the file will be placed inside it — check directory
  // owner via stat
  if (dst_ai_user.empty() && fs::is_directory(dst_path)) {
    std::string dir_owner = core::PathResolver::detect_owner_user(dst_path);
    if (!dir_owner.empty() &&
        validate_ai_user_ownership(dir_owner, main_user,
                                   ctx.config.user.prefix)) {
      dst_ai_user = dir_owner;
    }
  }

  // --- Source ai-user security check ---
  // If src belongs to an ai-user, verify it belongs to the current main-user
  if (!src_ai_user.empty()) {
    if (!validate_ai_user_ownership(src_ai_user, main_user,
                                    ctx.config.user.prefix)) {
      std::cerr << "Source belongs to ai-user '" << src_ai_user
                << "' which does not belong to user '" << main_user << "'"
                << std::endl;
      return 1;
    }
  }

  // --- Destination ai-user security check ---
  if (!dst_ai_user.empty()) {
    if (!validate_ai_user_ownership(dst_ai_user, main_user,
                                    ctx.config.user.prefix)) {
      std::cerr << "Destination ai-user '" << dst_ai_user
                << "' does not belong to user '" << main_user << "'"
                << std::endl;
      return 1;
    }
  }

  // --- Cross main-user leak prevention (Scenario D) ---
  // If src is ai-user and dst is a different ai-user, ensure both belong to
  // same main-user
  if (!src_ai_user.empty() && !dst_ai_user.empty() &&
      src_ai_user != dst_ai_user) {
    // Both have already passed validate_ai_user_ownership against main_user,
    // so they belong to the same main-user. This is safe (Scenario B).
  }

  // --- Determine chown target ---
  // Scenario A: main→ai → chown to dst_ai_user
  // Scenario B: ai→ai (same main) → chown to dst_ai_user
  // Scenario C: ai→main → chown back to main_user
  // Scenario D: cross-main → already rejected above
  // Scenario E: main→main → no chown needed
  bool need_chown = false;
  std::string chown_user;

  if (!dst_ai_user.empty()) {
    // Scenarios A/B: destination is ai-user territory
    need_chown = true;
    chown_user = dst_ai_user;
  } else if (!src_ai_user.empty()) {
    // Scenario C: moving from ai-user to main-user territory
    need_chown = true;
    chown_user = main_user;
  }
  // else: Scenario E, no chown needed

  std::error_code ec;
  fs::rename(src_path, dst_path, ec);
  if (ec) {
    auto cp_result = utils::exec_safe({"cp", "-rP", "--no-preserve=mode",
                                       src_path.string(), dst_path.string()});
    if (cp_result.exit_code != 0) {
      std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
      return 1;
    }

    if (need_chown) {
      fs::path chown_target = fs::is_directory(dst_path)
                                  ? dst_path / src_path.filename()
                                  : dst_path;
      if (!safe_chown_path(chown_target, chown_user)) {
        std::cerr << "Failed to set ownership after copy" << std::endl;
        return 1;
      }
    }

    struct stat src_stat;
    if (lstat(src_path.c_str(), &src_stat) != 0) {
      utils::get_logger()->warn("Failed to stat source after copy: {}",
                                strerror(errno));
    } else if (S_ISLNK(src_stat.st_mode)) {
      if (unlink(src_path.c_str()) != 0) {
        utils::get_logger()->warn("Failed to unlink symlink source: {}",
                                  strerror(errno));
      }
    } else if (S_ISDIR(src_stat.st_mode)) {
      int srcfd = open(src_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (srcfd < 0) {
        if (errno == ELOOP) {
          utils::get_logger()->warn(
              "Source became symlink, refusing recursive delete: {}",
              src_path.string());
        } else {
          fs::remove_all(src_path, ec);
          if (ec) {
            utils::get_logger()->warn("Failed to remove source directory: {}",
                                      ec.message());
          }
        }
      } else {
        auto remove_result = fs::remove_all(src_path);
        if (remove_result == static_cast<std::uintmax_t>(-1)) {
          utils::get_logger()->warn("Failed to remove source directory");
        }
        close(srcfd);
      }
    } else {
      if (unlink(src_path.c_str()) != 0) {
        utils::get_logger()->warn("Failed to remove source file: {}",
                                  strerror(errno));
      }
    }

    if (verbose) {
      if (need_chown) {
        std::cout << "Moved (copy+delete): " << src_path.string() << " -> "
                  << dst_path.string() << " (owner: " << chown_user << ")"
                  << std::endl;
      } else {
        std::cout << "Moved (copy+delete): " << src_path.string() << " -> "
                  << dst_path.string() << std::endl;
      }
    }
    return 0;
  }

  if (need_chown) {
    fs::path chown_target =
        fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
    if (!safe_chown_path(chown_target, chown_user)) {
      utils::get_logger()->error(
          "Rename succeeded but chown failed for {}, attempting rollback",
          chown_target.string());
      std::error_code rollback_ec;
      for (int retry = 0; retry < 3; ++retry) {
        fs::rename(dst_path, src_path, rollback_ec);
        if (!rollback_ec)
          break;
      }
      if (rollback_ec) {
        utils::get_logger()->error("Rollback failed after 3 retries: {} - "
                                   "MANUAL INTERVENTION REQUIRED for {}",
                                   rollback_ec.message(), dst_path.string());
      }
      std::cerr << "Failed to set ownership after atomic rename" << std::endl;
      return 1;
    }
  }

  if (verbose) {
    if (need_chown) {
      std::cout << "Moved (atomic): " << src_path.string() << " -> "
                << dst_path.string() << " (owner: " << chown_user << ")"
                << std::endl;
    } else {
      std::cout << "Moved (atomic): " << src_path.string() << " -> "
                << dst_path.string() << std::endl;
    }
  }
  return 0;
}

// Execute ssh in a forked child process, connecting stdin/stdout/stderr
// directly to the terminal. This replaces the old am.sh ssh -tt wrapper.
static int exec_ssh_interactive(const std::string &ai_user,
                                const std::string &target_path,
                                const std::string &ssh_key,
                                const std::string &known_hosts) {
  std::string escaped_path = target_path;
  // Escape single quotes for the remote command
  // Replace ' with '\''
  size_t pos = 0;
  while ((pos = escaped_path.find('\'', pos)) != std::string::npos) {
    escaped_path.replace(pos, 1, "'\\''");
    pos += 4;
  }

  // Remote command: configure git safe.directory, cd, then interactive shell
  std::string remote_cmd = "git config --global --add safe.directory '";
  remote_cmd += escaped_path;
  remote_cmd += "' 2>/dev/null || true; cd '";
  remote_cmd += escaped_path;
  remote_cmd += "' && exec bash -l";

  std::string host = ai_user + "@localhost";

  // Build argv: ssh -tt -i key -o IdentitiesOnly=yes -o
  // StrictHostKeyChecking=accept-new -o UserKnownHostsFile=... user@host
  // remote_cmd
  std::vector<std::string> args_str = {
      "/usr/bin/ssh", "-tt",
      "-o",           "ConnectTimeout=10",
      "-o",           "ConnectionAttempts=1",
      "-o",           "ServerAliveInterval=5",
      "-o",           "ServerAliveCountMax=3",
      "-i",           ssh_key,
      "-o",           "IdentitiesOnly=yes",
      "-o",           "StrictHostKeyChecking=accept-new",
      "-o",           "UserKnownHostsFile=" + known_hosts,
      host,           remote_cmd};

  std::vector<char *> argv;
  argv.reserve(args_str.size() + 1);
  for (auto &a : args_str)
    argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) {
    std::cerr << "error: fork() failed: " << strerror(errno) << std::endl;
    return 1;
  }

  if (pid == 0) {
    // Child: exec ssh directly. stdin/stdout/stderr are inherited from parent.
    // No $() capture in shell function, so SSH has raw terminal access.
    ::execv("/usr/bin/ssh", argv.data());
    std::cerr << "error: execv(ssh) failed: " << strerror(errno) << std::endl;
    ::_exit(127);
  }

  // Parent: wait for ssh to finish (no timeout — user may stay logged in
  // indefinitely) Interactive SSH sessions have no time limit; let user decide
  // when to exit.
  int status = 0;
  ::waitpid(pid, &status, 0);

  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int cmd_cd(const std::string &path, [[maybe_unused]] bool verbose,
           bool dry_run) {
  auto config = core::ConfigParser::load_default();
  std::string prefix = config.user.prefix;

  std::string main_user = utils::get_effective_username();

  auto target_opt = core::PathResolver::resolve(path);
  if (!target_opt) {
    std::cerr << "Cannot resolve path: " << path << std::endl;
    return 1;
  }
  fs::path target = *target_opt;
  if (!fs::exists(target)) {
    std::cerr << "Path does not exist: " << path << std::endl;
    return 1;
  }

  std::string target_str = target.string();
  if (!utils::validate_path_no_shell_metachars(target_str)) {
    std::cerr << "Path contains disallowed characters" << std::endl;
    return 1;
  }

  if (target_str == "/") {
    std::cerr << "Path traversal detected: resolves to root directory"
              << std::endl;
    return 1;
  }

  if (!security::validate_mount_source(target)) {
    std::cerr << "Path not in allowed directory: " << target_str << std::endl;
    return 1;
  }

  // Primary method: find .am_status by walking up from target directory
  // This works for any filesystem (BeeGFS, NFS, local) regardless of home_dir
  // location
  fs::path search_path = target;
  std::optional<core::UserInfo> state;
  std::error_code ec;

  // Track the closest corrupted .am_status for the error message below
  fs::path corrupted_path;
  while (!search_path.empty() && search_path != "/" && search_path != "/home") {
    state = core::UserManager::read_state(search_path);
    if (state) {
      corrupted_path.clear(); // found valid state, clear any corruption hint
      break;
    }
    // If .am_status exists but read_state failed, record for error message
    std::error_code ec2;
    if (corrupted_path.empty() &&
        fs::exists(search_path / ".am_status", ec2) && !ec2) {
      corrupted_path = search_path;
    }
    search_path = search_path.parent_path();
  }

  if (state) {
    std::string ai_user = state->username;

    // NOTE: am cd must NOT perform any filesystem operations (stat, mount)
    // on the target mount path.  stat() on a stale BeeGFS bind mount target
    // enters uninterruptible D (uninterruptible sleep) state and hangs
    // indefinitely.  See issue 2026-06-23-stale-mount-d-state.
    //
    // However, we CAN do a safe in-memory check: compare the project's
    // config mount paths against the mounts recorded in .am_status.
    // If the config expects a mount that .am_status doesn't have, the user
    // needs to run 'am update' to sync the new mount.  This is a pure
    // string comparison — no stat(), no mount operations, no hang risk.
    {
      // Build set of mount TARGETS from .am_status
      std::unordered_set<std::string> status_targets;
      for (const auto &m : state->mounts) {
        status_targets.insert(m.target);
      }
      // Check config mount paths against status targets
      std::vector<std::string> missing;
      for (const auto &mp : config.mount.paths) {
        auto expected = core::PathResolver::to_ai_user_path(
            mp, ai_user, main_user, state->home_dir);
        if (status_targets.find(expected.string()) == status_targets.end()) {
          missing.push_back(mp.string());
        }
      }
      if (!missing.empty()) {
        std::cerr << "warning: " << missing.size()
                  << " mount(s) not configured for this project:" << std::endl;
        for (const auto &m : missing) {
          std::cerr << "  - " << m << std::endl;
        }
        std::cerr << "  Run 'am update " << target_str << "' to sync."
                  << std::endl;
      }
    }

    // Validate SSH key exists
    std::string ssh_key = config.ssh.key_path.string();
    if (ssh_key.empty()) {
      std::cerr << "error: no SSH key configured" << std::endl;
      return 1;
    }
    if (!fs::exists(ssh_key, ec)) {
      std::cerr << "error: SSH key missing: " << ssh_key << ". Run 'am update "
                << target_str << "' first." << std::endl;
      return 1;
    }

    // Validate path is under main_user's allowed directories
    if (!utils::is_path_allowed(target, main_user, config.user.allowed_bases)) {
      std::cerr << "error: invalid path: '" << target_str
                << "' (must be under $HOME or allowed bases)" << std::endl;
      return 1;
    }

    // Validate AI user format
    std::string expected_prefix = prefix + main_user + "_";
    if (ai_user.size() <= expected_prefix.size() ||
        ai_user.substr(0, expected_prefix.size()) != expected_prefix) {
      std::cerr << "error: invalid AI user: '" << ai_user << "' (expected "
                << "format: " << prefix << main_user << "_<hash>)" << std::endl;
      return 1;
    }

    // Check if key is in authorized_keys (debug info)
    fs::path ssh_dir = fs::path(state->home_dir) / ".ssh";
    fs::path auth_keys = ssh_dir / "authorized_keys";
    bool key_authorized = false;
    if (fs::exists(auth_keys, ec) && !ec) {
      std::ifstream ak(auth_keys);
      fs::path main_pub = fs::path(ssh_key + ".pub");
      std::ifstream pk(main_pub);
      if (pk.is_open()) {
        std::string pub_key_line, auth_line;
        std::getline(pk, pub_key_line);
        auto space = pub_key_line.find(' ');
        if (space != std::string::npos) {
          std::string key_type = pub_key_line.substr(0, space);
          while (std::getline(ak, auth_line)) {
            if (auth_line.find(key_type) != std::string::npos) {
              key_authorized = true;
              break;
            }
          }
        }
      }
    }

    // Debug line for SSH troubleshooting (only when key not authorized).
    // NOTE: only stat ai-user's ~/.ssh here (never bind-mount targets) —
    // stat() on a stale BeeGFS bind mount hangs in D state (see note above).
    if (!key_authorized) {
      bool ssh_dir_exists = fs::exists(ssh_dir, ec) && !ec;
      bool auth_keys_exists = fs::exists(auth_keys, ec) && !ec;
      std::string ssh_dir_perms = "missing";
      std::string auth_keys_perms = "missing";
      if (ssh_dir_exists) {
        auto st = fs::status(ssh_dir, ec);
        if (!ec)
          ssh_dir_perms = std::to_string(
              static_cast<unsigned>(st.permissions() & fs::perms::all));
      }
      if (auth_keys_exists) {
        auto st = fs::status(auth_keys, ec);
        if (!ec)
          auth_keys_perms = std::to_string(
              static_cast<unsigned>(st.permissions() & fs::perms::all));
      }
      std::cerr << "debug=home=" << state->home_dir
                << ",ssh_perms=" << ssh_dir_perms
                << ",auth_perms=" << auth_keys_perms
                << ",key_in_auth=" << (key_authorized ? "yes" : "no")
                << std::endl;
    }

    // Known hosts file: use main user's ~/.ssh/known_hosts
    std::string known_hosts =
        (fs::path(utils::get_effective_home()) / ".ssh" / "known_hosts")
            .string();

    // Convert main user's target path to ai-user's path
    // ai-user accesses files via bind mounts in their home directory
    fs::path ai_user_target = core::PathResolver::to_ai_user_path(
        target, ai_user, main_user, state->home_dir);
    std::string ai_user_target_str = ai_user_target.string();

    // Build remote command for SSH interactive session
    // Configure git safe.directory, cd to target path, then start login shell
    std::string escaped_ai_path = ai_user_target_str;
    size_t esc_pos = 0;
    while ((esc_pos = escaped_ai_path.find('\'', esc_pos)) !=
           std::string::npos) {
      escaped_ai_path.replace(esc_pos, 1, "'\\''");
      esc_pos += 4;
    }
    std::string remote_cmd = "git config --global --add safe.directory '";
    remote_cmd += escaped_ai_path;
    remote_cmd += "' 2>/dev/null || true; cd '";
    remote_cmd += escaped_ai_path;
    remote_cmd += "' && exec bash -l";

    // Dry-run: output JSON decision to stdout, do not exec SSH
    if (dry_run) {
      nlohmann::json j;
      j["action"] = "ssh";
      j["user"] = ai_user;
      j["path"] = target_str;
      j["ai_user_path"] = ai_user_target_str;
      j["remote_cmd"] = remote_cmd;
      j["ssh_key"] = ssh_key;
      j["known_hosts"] = known_hosts;
      std::cout << j.dump() << std::endl;
      return 0;
    }

    // Directly execute SSH in a forked child process
    // SSH connects to AI user, user gets interactive shell
    // This is the SSH path — we must fork+exec SSH directly
    return exec_ssh_interactive(ai_user, ai_user_target_str, ssh_key,
                                known_hosts);
  }

  // Fallback: detect AI user from path component name (legacy method)
  std::string ai_user =
      core::PathResolver::detect_ai_user_from_path(target, main_user, prefix);

  if (!ai_user.empty()) {
    // Legacy path-detected AI user: output JSON for shell to exec SSH
    // (no .am_status file found, so we don't have enough info to exec ssh
    // directly — shell must handle SSH execution)
    std::string ssh_key = config.ssh.key_path.string();
    std::string known_hosts =
        (fs::path(utils::get_effective_home()) / ".ssh" / "known_hosts")
            .string();

    if (dry_run) {
      // Legacy path: no .am_status, use main user's path for remote command
      std::string escaped_path = target_str;
      size_t esc_pos = 0;
      while ((esc_pos = escaped_path.find('\'', esc_pos)) !=
             std::string::npos) {
        escaped_path.replace(esc_pos, 1, "'\\''");
        esc_pos += 4;
      }
      std::string legacy_remote_cmd =
          "git config --global --add safe.directory '";
      legacy_remote_cmd += escaped_path;
      legacy_remote_cmd += "' 2>/dev/null || true; cd '";
      legacy_remote_cmd += escaped_path;
      legacy_remote_cmd += "' && exec bash -l";

      nlohmann::json j;
      j["action"] = "ssh";
      j["user"] = ai_user;
      j["path"] = target_str;
      j["remote_cmd"] = legacy_remote_cmd;
      j["ssh_key"] = ssh_key;
      j["known_hosts"] = known_hosts;
      std::cout << j.dump() << std::endl;
      return 0;
    }

    // Non-dry-run legacy: output key=value for backward compat
    std::cout << "action=ssh" << std::endl;
    std::cout << "user=" << ai_user << std::endl;
    std::cout << "path=" << target_str << std::endl;
    return 0;
  }

  // Local cd: output JSON decision to stdout.
  // Shell function parses JSON and executes cd.
  // No intermediate files, no /tmp/ operations, no stderr markers.
  {
    nlohmann::json j;
    j["action"] = "cd";
    j["path"] = target_str;
    std::cout << j.dump() << std::endl;
  }
  return 0;
}

int cmd_list(bool verbose) {
  auto ctx = make_context(verbose);
  auto users = ctx.user_mgr->list_ai_users();

  if (users.empty()) {
    std::cout << "No ai-mirror managed users found." << std::endl;
    return 0;
  }

  std::string main_user = utils::get_effective_username();
  std::string expected_prefix = ctx.config.user.prefix + main_user + "_";

  std::cout << "ai-mirror managed users:" << std::endl;
  for (const auto &u : users) {
    if (u.username.substr(0, expected_prefix.length()) != expected_prefix)
      continue;
    auto mounts = ctx.graft->list_mounts(u.username);
    std::cout << "  " << u.username << " (uid=" << u.uid
              << ", home=" << u.home_dir << ", mounts=" << mounts.size() << ")"
              << std::endl;
  }
  return 0;
}

int cmd_health([[maybe_unused]] bool verbose) {
  auto ctx = make_context(verbose);
  daemon::HealthCheck hc(ctx.config.user.prefix);
  auto statuses = hc.check_all();

  // Stale mount detection: find mounts not in current config
  // For each ai-user, build configured_targets from config, then check all
  // their mounts against it. Mounts not in any user's config are stale.
  auto users = ctx.user_mgr->list_ai_users();
  std::string main_user = utils::get_effective_username();
  std::string expected_prefix = ctx.config.user.prefix + main_user + "_";

  // Build a map: ai-user home -> username (only for current main_user's
  // ai-users)
  std::vector<std::pair<std::string, std::string>> user_home_to_name;
  for (const auto &u : users) {
    if (u.username.size() <= expected_prefix.size() ||
        u.username.substr(0, expected_prefix.size()) != expected_prefix)
      continue;
    if (!u.home_dir.empty()) {
      user_home_to_name.emplace_back(u.home_dir, u.username);
    }
  }

  // For each ai-user, build configured_targets set
  std::map<std::string, std::set<std::string>> user_configured_targets;
  for (const auto &[home_dir, username] : user_home_to_name) {
    std::set<std::string> targets;
    for (const auto &mount_path : ctx.config.mount.paths) {
      auto source_opt = core::PathResolver::resolve(mount_path.string());
      if (!source_opt)
        continue;
      fs::path source = *source_opt;
      if (!fs::exists(source))
        continue;
      fs::path target = core::PathResolver::to_ai_user_path(
          source, username, main_user, home_dir);
      targets.insert(target.string());
    }
    user_configured_targets[username] = std::move(targets);
  }

  // Get all mounts for each ai-user and check for stale ones
  for (const auto &[home_dir, username] : user_home_to_name) {
    auto user_mounts = ctx.graft->list_mounts(username);
    auto &configured = user_configured_targets[username];

    for (const auto &m : user_mounts) {
      std::string target_str = m.target.string();
      if (!configured.count(target_str)) {
        // This mount is not in current config — mark as stale
        daemon::HealthStatus stale_status;
        stale_status.mount_point = target_str;
        stale_status.healthy = false;
        stale_status.stale = true;
        stale_status.detail =
            "Not in current config (run 'am update' to clean)";
        statuses.push_back(stale_status);
      }
    }
  }

  // [root.md §2.3] AM home permission check: warn if group write is PRESENT
  // AI user home MUST be 0755 (owner rwx, group/other r-x), NO g+w
  // Main user operates via SSH, not via shared group write permission
  for (const auto &[home_dir, username] : user_home_to_name) {
    struct stat st;
    if (stat(home_dir.c_str(), &st) == 0) {
      if ((st.st_mode & S_IWGRP) != 0) {
        daemon::HealthStatus perm_status;
        perm_status.mount_point = home_dir;
        perm_status.healthy = true; // Not unhealthy, just a warning
        perm_status.stale = false;
        perm_status.detail = "AM home has g+w (violates root.md §2.3, run 'am "
                             "auto-fix-all' to fix)";
        statuses.push_back(perm_status);
      }
    }
  }

  if (statuses.empty()) {
    std::cout << "No mounts to check." << std::endl;
    return 0;
  }

  int unhealthy = 0;
  for (const auto &s : statuses) {
    std::string status = s.stale ? "STALE" : (s.healthy ? "OK" : "FAIL");
    std::cout << "[" << status << "] " << s.mount_point << " - " << s.detail
              << std::endl;
    if (!s.healthy)
      unhealthy++;
  }

  // Permission warnings don't affect return code - they're advisory
  // Unhealthy mounts still cause return 1
  return unhealthy > 0 ? 1 : 0;
}

// Forward declaration (defined after cmd_force_destroy)
static bool terminate_user_processes(const std::string &username);

int cmd_force_destroy(const std::string &project_or_user, bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror force-destroy requires root privileges"
              << std::endl;
    return 1;
  }

  std::string username = project_or_user;
  if (!utils::validate_username(username)) {
    // Try reading from .am_status first (backward compatibility)
    auto proj_opt = core::PathResolver::resolve(project_or_user);
    if (proj_opt) {
      auto state = core::UserManager::read_state(*proj_opt);
      if (state && !state->username.empty()) {
        username = state->username;
        utils::get_logger()->info(
            "cmd_force_destroy: using username '{}' from .am_status", username);
      }
    }
    // If still not valid, derive from path hash
    if (!utils::validate_username(username)) {
      auto derived = ctx.user_mgr->derive_username(project_or_user);
      if (!derived) {
        std::cerr << "Cannot derive valid username for: " << project_or_user
                  << std::endl;
        return 1;
      }
      username = std::move(*derived);
    }
  }
  if (!ctx.user_mgr->user_exists(username)) {
    std::cerr << "User not found: " << username << std::endl;
    return 1;
  }

  std::string main_user = utils::get_effective_username();
  if (!validate_ai_user_ownership(username, main_user,
                                  ctx.config.user.prefix)) {
    std::cerr << "User '" << username << "' does not belong to '" << main_user
              << "'" << std::endl;
    return 1;
  }

  utils::get_logger()->warn("Force destroying user: {}", username);

  // Terminate user processes (force-destroy must be aggressive)
  utils::get_logger()->info("Killing processes for user {}", username);
  terminate_user_processes(username);

  // Clear crontab to prevent respawn
  utils::exec_safe({"crontab", "-r", "-u", username});

  daemon::MountCleaner cleaner(ctx.config.user.prefix);
  cleaner.cleanup_for_user(username);

  if (!ctx.user_mgr->remove_ai_user(username, true)) {
    std::cerr << "Failed to remove user: " << username << std::endl;
    return 1;
  }

  std::cout << "Destroyed: " << username << std::endl;
  return 0;
}

static bool terminate_user_processes(const std::string &username) {
  auto logger = utils::get_logger();

  /// Round 1: SIGTERM — graceful shutdown
  logger->info("Round 1: SIGTERM to processes of user {}", username);
  utils::exec_safe({"pkill", "-u", username});
  usleep(1000000); // 1s wait

  /// Check if processes remain
  auto ps_check = utils::exec_safe({"ps", "-u", username, "-o", "pid="});
  bool has_procs = (ps_check.exit_code == 0) && !ps_check.stdout_output.empty();

  if (!has_procs) {
    logger->info("All processes terminated for user {} after SIGTERM",
                 username);
    return true;
  }

  logger->warn("Processes still running for user {}, sending SIGKILL:\n{}",
               username, ps_check.stdout_output);

  /// Round 2: SIGKILL — force kill stubborn processes
  utils::exec_safe({"pkill", "-9", "-u", username});
  usleep(1000000); // 1s wait

  // Final verification
  ps_check = utils::exec_safe({"ps", "-u", username, "-o", "pid="});
  has_procs = (ps_check.exit_code == 0) && !ps_check.stdout_output.empty();

  if (has_procs) {
    logger->warn("Unkillable processes remain for user {}:\n{}", username,
                 ps_check.stdout_output);
    return false;
  }

  logger->info("All processes terminated for user {} after SIGKILL", username);
  return true;
}

int cmd_rm(const std::string &project_path, bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    utils::get_logger()->error("ai-mirror rm requires root privileges");
    std::cerr << "ai-mirror rm requires root privileges" << std::endl;
    return 1;
  }

  auto proj_opt = core::PathResolver::resolve(project_path);
  if (!proj_opt) {
    utils::get_logger()->error("Invalid project path: {}", project_path);
    std::cerr << "Invalid project path: " << project_path << std::endl;
    return 1;
  }
  fs::path proj = *proj_opt;

  // Priority 1: read username from .am_status (backward compatibility with old
  // format) Priority 2: derive username using new hash-based naming
  std::string username;
  std::string main_user = utils::get_effective_username();

  auto state = core::UserManager::read_state(proj);
  if (state && !state->username.empty()) {
    username = state->username;
    // Use main_user from state if available (more accurate)
    if (!state->main_user.empty()) {
      main_user = state->main_user;
    }
    utils::get_logger()->info("cmd_rm: using username '{}' from .am_status",
                              username);
  } else {
    // No .am_status, derive username using hash-based naming
    auto derived = ctx.user_mgr->derive_username(proj.string());
    if (!derived) {
      utils::get_logger()->error(
          "Username collision: cannot derive unique username for: {}",
          proj.string());
      std::cerr << "Username collision: cannot derive unique username for: "
                << proj.string() << std::endl;
      return 1;
    }
    username = std::move(*derived);
    utils::get_logger()->info("cmd_rm: derived username '{}' from path hash",
                              username);
  }

  if (!validate_ai_user_ownership(username, main_user,
                                  ctx.config.user.prefix)) {
    utils::get_logger()->error("ai_user '{}' does not belong to user '{}'",
                               username, main_user);
    std::cerr << "ai_user '" << username << "' does not belong to user '"
              << main_user << "'" << std::endl;
    return 1;
  }

  if (!ctx.user_mgr->user_exists(username)) {
    utils::get_logger()->error(
        "AI user not found for project: {} (expected: {})", proj.string(),
        username);
    std::cerr << "AI user not found for project: " << proj.string()
              << std::endl;
    std::cerr << "Expected user: " << username << std::endl;
    return 1;
  }

  auto user_info = ctx.user_mgr->get_user_info(username);
  if (!user_info) {
    utils::get_logger()->error("Failed to get user info: {}", username);
    std::cerr << "Failed to get user info: " << username << std::endl;
    return 1;
  }

  fs::path ai_home(user_info->home_dir);

  utils::get_logger()->info("Removing project: {} (user: {}, home: {})",
                            proj.string(), username, ai_home.string());

  // Step 1: Terminate user processes (must be before unmount)
  utils::get_logger()->info("Step 1: Terminating processes for user {}",
                            username);
  if (verbose) {
    std::cout << "Step 1: Terminating processes for " << username << std::endl;
  }
  if (!terminate_user_processes(username)) {
    utils::get_logger()->warn(
        "Some processes for user {} could not be killed, attempting userdel "
        "anyway",
        username);
  }

  // Step 1b: Clear user crontab (prevent process restart after kill)
  utils::get_logger()->info("Step 1b: Clearing crontab for user {}", username);
  if (verbose) {
    std::cout << "Step 1b: Clearing crontab for " << username << std::endl;
  }
  utils::exec_safe({"crontab", "-r", "-u", username});

  // Step 2: Unmount bind mounts
  utils::get_logger()->info("Step 2: Unmounting bind mounts for {}", username);
  if (verbose) {
    std::cout << "Step 2: Unmounting bind mounts for " << username << std::endl;
  }
  daemon::MountCleaner cleaner(ctx.config.user.prefix);
  int mounts_cleaned = cleaner.cleanup_for_user(username);
  utils::get_logger()->info("Unmounted {} mount(s) for {}", mounts_cleaned,
                            username);

  // Step 3: Remove Linux user (userdel)
  utils::get_logger()->info("Step 3: Removing Linux user {}", username);
  if (verbose) {
    std::cout << "Step 3: Removing user " << username << std::endl;
  }
  if (!ctx.user_mgr->remove_ai_user(username, false)) {
    utils::get_logger()->error("Failed to remove user: {}", username);
    std::cerr << "Failed to remove user: " << username << std::endl;
    return 1;
  }
  utils::get_logger()->info("User {} removed from system", username);

  // Step 4: Revoke write access on project directory (before deleting home)
  utils::get_logger()->info("Step 4: Revoking write access on project {}",
                            proj.string());
  if (verbose) {
    std::cout << "Step 4: Revoking write grants on project" << std::endl;
  }
  if (!ctx.graft->revoke_write_access(proj, username)) {
    utils::get_logger()->warn(
        "Failed to revoke write access for user '{}' on project '{}'", username,
        proj.string());
  } else {
    utils::get_logger()->info("Write access revoked for {} on {}", username,
                              proj.string());
  }

  // Step 5: Clean up ai-user home directory and .am_status
  utils::get_logger()->info("Step 5: Cleaning up ai-user home: {}",
                            ai_home.string());
  if (verbose) {
    std::cout << "Step 5: Cleaning up ai-user home" << std::endl;
  }
  {
    std::error_code ec;
    fs::remove_all(ai_home, ec);
    if (ec) {
      utils::get_logger()->warn("Failed to clean home dir {}: {}",
                                ai_home.string(), ec.message());
    } else {
      utils::get_logger()->info("Removed home directory: {}", ai_home.string());
    }
  }

  // Remove .am_status from project directory
  fs::path status_file = proj / ".am_status";
  if (fs::exists(status_file)) {
    std::error_code ec;
    fs::remove(status_file, ec);
    if (ec) {
      utils::get_logger()->warn("Failed to remove .am_status: {}",
                                ec.message());
    } else {
      utils::get_logger()->info("Removed .am_status from project: {}",
                                proj.string());
    }
  }

  utils::get_logger()->info("Removed: {} (project: {})", username,
                            proj.string());
  std::cout << "Removed: " << username << std::endl;
  return 0;
}

int cmd_config([[maybe_unused]] bool verbose) {
  auto config = core::ConfigParser::load_default();

  std::cout << "Config file: " << config.config_path.string() << std::endl;
  std::cout << "User prefix: " << config.user.prefix << " (default)"
            << std::endl;

  if (!config.user.allowed_bases.empty()) {
    std::cout << "Allowed bases:" << std::endl;
    for (const auto &b : config.user.allowed_bases) {
      std::cout << "  - " << b.string() << std::endl;
    }
  }

  std::cout << "SSH key type: " << config.ssh.key_type << std::endl;
  std::cout << "SSH key path: " << config.ssh.key_path.string() << std::endl;
  std::cout << "SSH default key: " << config.ssh.ai_default_key.string()
            << std::endl;

  std::cout << "Mount paths:" << std::endl;
  for (const auto &p : config.mount.paths) {
    std::cout << "  - " << p.string() << std::endl;
  }

  if (!config.ai_user.groups.empty()) {
    std::cout << "AI-user groups:" << std::endl;
    for (const auto &g : config.ai_user.groups) {
      std::cout << "  - " << g << std::endl;
    }
  }

  std::cout << "Loaded: " << (config.loaded ? "yes" : "no (using defaults)")
            << std::endl;

  return 0;
}
int cmd_status([[maybe_unused]] bool verbose) {
  auto ctx = make_context(verbose);
  auto users = ctx.user_mgr->list_ai_users();

  if (users.empty()) {
    std::cout << "No ai-mirror managed projects." << std::endl;
    return 0;
  }

  std::string main_user = utils::get_effective_username();
  std::string expected_prefix = ctx.config.user.prefix + main_user + "_";

  for (const auto &u : users) {
    if (u.username.substr(0, expected_prefix.length()) != expected_prefix)
      continue;
    std::cout << "Project: " << u.username << std::endl;
    std::cout << "  Home:  " << u.home_dir << std::endl;
    std::cout << "  UID:   " << u.uid << std::endl;
    if (!u.project_path.empty()) {
      std::cout << "  Path:  " << u.project_path << std::endl;
    }
    if (!u.path_hash.empty()) {
      std::cout << "  Hash:  " << u.path_hash << std::endl;
    }

    bool all_healthy = true;

    auto mounts = ctx.graft->list_mounts(u.username);
    if (mounts.empty()) {
      std::cout << "  Mounts: none" << std::endl;
    } else {
      std::cout << "  Mounts:" << std::endl;
      for (const auto &m : mounts) {
        std::string state = m.active ? "active" : "broken";
        std::string mode = m.read_only ? "ro" : "rw";
        std::cout << "    " << m.source.string() << " -> " << m.target.string()
                  << " (" << mode << ", " << state << ")" << std::endl;
        if (!m.active)
          all_healthy = false;
      }
    }

    fs::path key_path = ctx.config.ssh.key_path;
    if (key_path.string().size() >= 2 && key_path.string()[0] == '~' &&
        key_path.string()[1] == '/') {
      key_path =
          fs::path(utils::get_effective_home()) / key_path.string().substr(2);
    }
    bool ssh_ok = fs::exists(key_path) &&
                  fs::exists(fs::path(key_path.string() + ".pub"));
    std::cout << "  SSH:   " << (ssh_ok ? "ok" : "missing") << std::endl;
    if (!ssh_ok)
      all_healthy = false;

    fs::path auth_keys = fs::path(u.home_dir) / ".ssh" / "authorized_keys";
    std::cout << "  Auth:  " << (fs::exists(auth_keys) ? "ok" : "missing")
              << std::endl;
    if (!fs::exists(auth_keys))
      all_healthy = false;

    if (mounts.empty())
      all_healthy = false;

    std::cout << "  Status: " << (all_healthy ? "healthy" : "unhealthy")
              << std::endl;
    std::cout << std::endl;
  }

  return 0;
}

int cmd_update(const std::string &path, [[maybe_unused]] bool verbose) {
  auto ctx = make_context(verbose);

  auto target_opt = core::PathResolver::resolve(path);
  if (!target_opt) {
    std::cerr << "Cannot resolve path: " << path << std::endl;
    return 1;
  }
  fs::path proj = *target_opt;
  if (!fs::exists(proj)) {
    std::cerr << "Path does not exist: " << path << std::endl;
    return 1;
  }

  // Helper: extract main_user from AI username ({prefix}{user}_{hash})
  // Returns empty string if format is invalid.
  auto extract_main_user = [&](const std::string &ai_user) -> std::string {
    std::string p = ctx.config.user.prefix;
    if (ai_user.size() <= p.size() + 1)
      return {};
    auto pos = ai_user.find('_', p.size());
    if (pos == std::string::npos)
      return {};
    return ai_user.substr(p.size(), pos - p.size());
  };

  auto state = core::UserManager::read_state(proj);
  if (!state) {
    // read_state already logged the specific error.  Try recovery:
    // if .am_status exists but is corrupted, regenerate it from authoritative
    // sources: /etc/passwd (username/uid/gid), project path (path_hash),
    // /proc/mounts (mount list).
    auto st = fs::status(proj / ".am_status");
    if (fs::exists(st)) {
      // Step 1: Find home_dir (parent with .am_status, may be proj itself)
      fs::path home_dir;
      {
        fs::path p2 = proj;
        while (p2.has_parent_path() && p2 != p2.parent_path()) {
          if (fs::exists(p2 / ".am_status")) {
            home_dir = p2;
            break;
          }
          p2 = p2.parent_path();
        }
      }

      if (home_dir.empty()) {
        std::cerr << "  .am_status exists at " << proj.string()
                  << " but no parent directory has one (walk-up failed)"
                  << std::endl;
      } else {
        // Step 2: Find AI user by home directory in /etc/passwd
        std::string ai_username;
        setpwent();
        while (auto *pw = getpwent()) {
          if (home_dir == fs::path(pw->pw_dir)) {
            ai_username = pw->pw_name;
            break;
          }
        }
        endpwent();

        if (ai_username.empty()) {
          std::cerr << "  No AI user found in /etc/passwd with home="
                    << home_dir.string() << std::endl;
          std::cerr << "  The AI user may have been deleted manually."
                    << std::endl;
          std::cerr << "  Fix: run 'am create " << proj.string()
                    << "' to re-create." << std::endl;
        } else {
          // Step 3: Extract main_user from username format
          std::string main_user = extract_main_user(ai_username);
          if (main_user.empty()) {
            std::cerr << "  AI user '" << ai_username
                      << "' has unexpected format" << std::endl;
            std::cerr << "  Expected: " << ctx.config.user.prefix
                      << "{main_user}_{hash}" << std::endl;
            std::cerr << "  Fix: run 'am create " << proj.string()
                      << "' to re-create." << std::endl;
          } else if (!core::UserManager::rebuild_state(
                         home_dir, ai_username, main_user, proj,
                         ctx.config.user.prefix)) {
            std::cerr << "  Failed to rebuild .am_status for " << ai_username
                      << " (see log for details)" << std::endl;
            std::cerr << "  Fix: run 'am create " << proj.string()
                      << "' to re-create." << std::endl;
          } else {
            utils::get_logger()->info(
                "Recovered .am_status for {} from authoritative sources",
                ai_username);
            state = core::UserManager::read_state(proj);
          }
        }
      }
    }

    if (!state) {
      if (!fs::exists(st)) {
        std::cerr << "Not an ai-mirror project (no .am_status): "
                  << proj.string() << std::endl;
      } else {
        std::cerr << "Cannot recover .am_status for " << proj.string()
                  << std::endl;
      }
      return 1;
    }
  }

  std::string main_user = state->main_user.empty()
                              ? utils::get_effective_username()
                              : state->main_user;

  return do_configure(ctx, *state, proj, main_user);
}

// ============================================================================
// cmd_auto_fix_all: check all mounts and fix unhealthy ones via update
// ============================================================================

// Walk up from mount target to find project directory containing .am_status
static fs::path find_project_from_mount(const fs::path &mount_target) {
  fs::path p = mount_target;
  while (p.has_parent_path() && p != p.parent_path()) {
    std::error_code ec;
    if (fs::exists(p / ".am_status", ec)) {
      return p;
    }
    p = p.parent_path();
  }
  return {};
}

int cmd_auto_fix_all(bool verbose) {
  auto ctx = make_context(verbose);

  if (!utils::is_root()) {
    std::cerr << "ai-mirror auto-fix-all requires root privileges" << std::endl;
    return 1;
  }

  // Step 1: Get all unhealthy mounts (including stale mounts with inode
  // mismatch)
  core::Graft graft(ctx.config.user.prefix);
  auto unhealthy = graft.health_check();

  if (unhealthy.empty()) {
    std::cout << "All mounts healthy, nothing to fix." << std::endl;
    return 0;
  }

  std::cout << "Found " << unhealthy.size()
            << " unhealthy mount(s):" << std::endl;
  for (const auto &m : unhealthy) {
    std::cout << "  [FAIL] " << m.target.string() << std::endl;
  }

  // Step 2: Find unique project paths from unhealthy mount targets
  // NOTE: We do NOT unmount unhealthy mounts before fixing.
  // Unmounting first creates a race condition: if cmd_update fails,
  // the mount is already gone with no rollback.
  std::set<fs::path> projects_to_fix;
  for (const auto &m : unhealthy) {
    auto proj = find_project_from_mount(m.target);
    if (!proj.empty()) {
      projects_to_fix.insert(proj);
    } else {
      std::cerr << "Warning: cannot find project for mount target: "
                << m.target.string() << std::endl;
    }
  }

  if (projects_to_fix.empty()) {
    std::cerr << "No projects found to fix." << std::endl;
    return 1;
  }

  // Step 3: Fix each project's mounts via mount-only do_configure
  // [P1-performance] Use mount_only=true to skip SSH/groups/permissions setup.
  // auto-fix-all's sole job is fixing mounts — not reconfiguring the project.
  std::cout << "Fixing " << projects_to_fix.size()
            << " project(s) (mounts only):" << std::endl;
  int failures = 0;
  for (const auto &proj : projects_to_fix) {
    std::cout << "\n=== Fixing mounts for: " << proj.string()
              << " ===" << std::endl;
    auto state = core::UserManager::read_state(proj);
    if (!state) {
      auto status = fs::status(proj / ".am_status");
      if (!fs::exists(status)) {
        std::cerr << "  Skipped (no .am_status): " << proj.string()
                  << std::endl;
      } else if (fs::file_size(proj / ".am_status") == 0) {
        std::cerr << "  Skipped (empty .am_status): " << proj.string()
                  << std::endl;
      } else {
        std::cerr << "  Skipped (corrupted .am_status, see log): "
                  << proj.string() << std::endl;
      }
      failures++;
      continue;
    }
    std::string main_user = state->main_user.empty()
                                ? utils::get_effective_username()
                                : state->main_user;
    if (do_configure(ctx, *state, proj, main_user, true) != 0) {
      std::cerr << "Failed to fix mounts for: " << proj.string() << std::endl;
      failures++;
    } else {
      std::cout << "Fixed mounts for: " << proj.string() << std::endl;
    }
  }

  // Step 4: Re-check health after fixes
  std::cout << "\n=== Re-checking health ===" << std::endl;
  graft.invalidate_cache();
  auto remaining = graft.health_check();
  if (remaining.empty()) {
    std::cout << "All mounts healthy after fix." << std::endl;
    return 0;
  }

  std::cout << remaining.size() << " mount(s) still unhealthy:" << std::endl;
  for (const auto &m : remaining) {
    std::cout << "  [FAIL] " << m.target.string() << std::endl;
  }
  return 1;
}

// ============================================================================
// cmd_watch: FTXUI-based real-time monitoring for ai-users
// ============================================================================

namespace {
using namespace ftxui;

// Format memory with auto unit selection
std::string format_memory(unsigned long mb) {
  unsigned long kb = mb * 1024;
  std::ostringstream ss;
  if (kb < 1024) {
    ss << kb << "K";
  } else if (kb < 1024 * 1024) {
    ss << std::fixed << std::setprecision(1) << (kb / 1024.0) << "M";
  } else {
    ss << std::fixed << std::setprecision(2) << (kb / (1024.0 * 1024.0)) << "G";
  }
  return ss.str();
}

// Pad string to fixed width (truncate if too long)
std::string pad_col(const std::string &s, size_t width) {
  if (s.size() >= width)
    return s.substr(0, width);
  return s + std::string(width - s.size(), ' ');
}

// Build FTXUI table from stats (sorted: active users first)
Element render_stats_table(std::vector<daemon::UserStats> stats) {
  // Sort: logged_in users first, then by CPU%
  std::stable_sort(stats.begin(), stats.end(),
                   [](const daemon::UserStats &a, const daemon::UserStats &b) {
                     if (a.logged_in != b.logged_in)
                       return a.logged_in > b.logged_in;
                     return a.cpu_percent > b.cpu_percent;
                   });

  // Column widths: Username=24, CPU%=8, Mem=10, Procs=6, SSH=8
  auto header = hbox({
      text(pad_col("Username", 24)) | bold,
      text(pad_col("CPU%", 8)) | bold,
      text(pad_col("Mem", 10)) | bold,
      text(pad_col("Procs", 6)) | bold,
      text(pad_col("SSH", 8)) | bold,
  });

  std::vector<Element> rows;
  rows.push_back(header);
  rows.push_back(text(std::string(56, '-')) | dim); // underline below header

  size_t row_idx = 0;
  for (const auto &s : stats) {
    std::ostringstream cpu_ss;
    cpu_ss << std::fixed << std::setprecision(1) << s.cpu_percent;

    // CPU color: gradient from gray → green → yellow → red
    Color cpu_color;
    if (s.cpu_percent > 80)
      cpu_color = Color::Red;
    else if (s.cpu_percent > 60)
      cpu_color = Color::OrangeRed1;
    else if (s.cpu_percent > 40)
      cpu_color = Color::Yellow;
    else if (s.cpu_percent > 20)
      cpu_color = Color::Green;
    else if (s.cpu_percent > 5)
      cpu_color = Color::GreenLight;
    else
      cpu_color = Color::GrayLight;

    // Username: bold if active
    auto name_elem = s.logged_in ? text(pad_col(s.username, 24)) | bold
                                 : text(pad_col(s.username, 24));

    // SSH: light green + bold if active
    Element ssh_elem = s.logged_in ? text(pad_col("active", 8)) |
                                         color(Color::GreenLight) | bold
                                   : text(pad_col("no", 8)) | dim;

    rows.push_back(hbox({
        name_elem,
        text(pad_col(cpu_ss.str(), 8)) | color(cpu_color),
        text(pad_col(format_memory(s.memory_mb), 10)),
        text(pad_col(std::to_string(s.process_count), 6)),
        std::move(ssh_elem),
    }));
    row_idx++;

    // Separator every 5 rows for visual grouping
    if (row_idx % 5 == 0 && row_idx < stats.size()) {
      rows.push_back(text(std::string(56, '-')) | dim);
    }
  }

  return vbox(rows) | border;
}
} // anonymous namespace

int cmd_watch(const std::string &watch_path, const std::string &watch_user,
              bool verbose) {
  auto ctx = make_context(verbose);
  std::string main_user = utils::get_effective_username();

  // Resolve optional path filter
  std::optional<std::string> path_filter;
  if (!watch_path.empty()) {
    auto resolved = core::PathResolver::resolve(watch_path);
    if (!resolved) {
      std::cerr << "Invalid path: " << watch_path << std::endl;
      return 1;
    }
    path_filter = resolved->string();
  }

  // Print startup message (for test compatibility)
  std::cout << "Starting ai-mirror watch... Press Ctrl+C to exit.\n";

  auto screen = ScreenInteractive::Fullscreen();

  // Shared state for periodic data refresh
  auto stats = std::make_shared<std::vector<daemon::UserStats>>();
  auto empty_msg = std::make_shared<bool>(true);
  auto refresh_interval = std::make_shared<int>(5); // Default 5 seconds

  // Cached user list — only refresh every 30s to avoid re-reading /etc/passwd
  constexpr auto user_list_ttl = std::chrono::seconds(30);
  auto cached_usernames = std::make_shared<std::vector<std::string>>();
  auto cached_uids = std::make_shared<std::vector<uid_t>>();
  auto last_user_refresh =
      std::make_shared<std::chrono::steady_clock::time_point>();

  // Periodic data refresh via a separate thread
  std::atomic<bool> running{true};
  std::thread refresh_thread([&]() {
    while (running) {
      auto now = std::chrono::steady_clock::now();

      // Refresh user list only every 30 seconds
      bool need_user_refresh =
          !last_user_refresh || (now - *last_user_refresh) >= user_list_ttl;

      if (need_user_refresh) {
        auto users = ctx.user_mgr->list_ai_users();
        std::string expected_prefix = ctx.config.user.prefix + main_user + "_";
        std::vector<std::string> usernames;
        std::vector<uid_t> uids;

        for (const auto &u : users) {
          if (u.username.size() > expected_prefix.size() &&
              u.username.substr(0, expected_prefix.size()) == expected_prefix) {

            // Filter by user name if provided
            if (!watch_user.empty() && u.username != watch_user) {
              continue;
            }

            // Filter by project path if provided
            if (path_filter) {
              // User's home should match project path
              if (u.home_dir != *path_filter) {
                continue;
              }
            }

            usernames.push_back(u.username);
            uids.push_back(u.uid);
          }
        }

        *cached_usernames = std::move(usernames);
        *cached_uids = std::move(uids);
        *last_user_refresh = now;
      }

      if (cached_usernames->empty()) {
        *empty_msg = true;
        stats->clear();
      } else {
        *empty_msg = false;
        // Single /proc scan + single ps call for all users
        auto uid_stats = daemon::gather_all_uid_stats();
        *stats = daemon::build_user_stats(*cached_usernames, *cached_uids,
                                          uid_stats);
      }

      screen.PostEvent(Event::Custom);

      // Sleep with interrupt check (100ms granularity)
      for (int i = 0; i < *refresh_interval * 10 && running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });

  auto renderer = Renderer([&, stats, empty_msg, main_user, refresh_interval] {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::ostringstream time_ss;
    time_ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");

    if (*empty_msg) {
      return vbox({
                 text("No ai-users found for " + main_user) |
                     color(Color::Yellow) | bold | hcenter,
                 text("Create one with: am create <project_path>") | dim |
                     hcenter,
             }) |
             border | flex;
    }

    return vbox({
               hbox({
                   text("ai-mirror watch - " + main_user + "'s ai-users") |
                       bold | color(Color::Cyan),
                   filler(),
                   text("Press q/Esc to exit") | dim,
               }),
               separator(),
               render_stats_table(*stats),
               separator(),
               hbox({
                   text("Refresh: " + std::to_string(*refresh_interval) + "s") |
                       color(Color::Cyan),
                   text(" | "),
                   text(std::to_string(stats->size()) + " users") |
                       color(Color::Cyan),
                   text(" | "),
                   text("Last update: " + time_ss.str()) | color(Color::Cyan),
               }),
           }) |
           border | flex;
  });

  auto component = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    // FTXUI handles Ctrl+C automatically via SIGINT
    return false;
  });

  screen.Loop(component);

  running = false;
  refresh_thread.join();

  // Print exit message (for test compatibility)
  std::cout << "Watch stopped.\n";
  std::cout.flush();

  return 0;
}

// cmd_init: Output shell integration function (zoxide-style)
// Usage: eval "$(am init bash)"
//
// The shell function wraps the `am` binary:
// - For `am cd`: captures stdout, executes cd/ssh as needed
// - For all other commands: passes through to the binary directly
// - Handles newgrp hints
//
// Design: function name = binary name (same as pyenv/fzf)
// - No recursion: function uses absolute path to call binary
// - `am --type` shows diagnostic info
// - `unset -f am` restores raw binary
//
int cmd_init(const std::string &shell, [[maybe_unused]] bool verbose) {
  if (shell != "bash") {
    std::cerr << "error: unsupported shell: " << shell
              << " (only 'bash' is supported)" << std::endl;
    return 1;
  }

  // Output a self-contained shell function.
  // All logic lives in the C++ binary; this function is minimal glue.
  std::cout << R"DELIM(#!/usr/bin/env bash
# am shell integration — generated by 'am init bash'
# Install: add to ~/.bashrc:  eval "$(am init bash)"
#
# This function provides shell-level features that a binary cannot:
#   - 'am cd' local: changes directory in the current shell
#   - 'am cd' remote: SSH to ai-user and cd to target path (via remote_cmd)
#   - 'am --type': shows whether am is a function or binary
#
# Design: function overrides binary (same name). No recursion because
# function uses absolute path to call the wrapper binary.
# Restore binary: unset -f am

_am() {
	local _am_bin="/usr/local/bin/am"
	if [[ ! -x "$_am_bin" ]]; then
		echo "error: am binary not found at $_am_bin" >&2
		return 1
	fi

	# Diagnostic: show what 'am' currently is
	if [[ "${1:-}" == "--type" ]]; then
		echo "am is a shell function (overrides ${_am_bin})"
		echo "  Binary:   ${_am_bin}"
		echo "  Wrapper:  /usr/local/bin/ai-mirror-bin"
		echo "  Remove:   unset -f am"
		return 0
	fi

	# Pass through to binary.
	# For 'cd': two-phase approach — dry-run gets JSON decision, then execute.
	#   No intermediate files, no /tmp/ operations, no stderr markers.
	# For other commands: pass through directly (preserves SSH raw terminal).
	if [[ "$1" == "cd" ]]; then
		# Phase 1: get decision as JSON from binary (dry-run)
		# Pass all original args + --dry-run to binary
		# Binary outputs JSON: {"action":"cd","path":"..."} or
		#                      {"action":"ssh","user":"...","path":"...",
		#                       "ai_user_path":"...","remote_cmd":"...",
		#                       "ssh_key":"...","known_hosts":"..."}
		local _am_json
		_am_json=$("$_am_bin" "$@" --dry-run 2>/dev/null)
		local ret=$?

		if [[ $ret -ne 0 ]]; then
			# Binary reported error — re-run without --dry-run to show errors
			"$_am_bin" "$@"
			return $?
		fi

		# Phase 2: parse JSON and execute
		# Extract fields using Python (robust JSON parsing, no intermediate files)
		local _am_action _am_path _am_user _am_ssh_key _am_known_hosts _am_remote_cmd
		eval "$(echo "$_am_json" | python3 -c "
import json, shlex, sys
try:
    d = json.load(sys.stdin)
    a = d.get('action', '')
    print(f'_am_action={shlex.quote(a)}')
    print(f'_am_path={shlex.quote(d.get(\"path\", \"\"))}')
    if a == 'ssh':
        print(f'_am_user={shlex.quote(d.get(\"user\", \"\"))}')
        print(f'_am_ssh_key={shlex.quote(d.get(\"ssh_key\", \"\"))}')
        print(f'_am_known_hosts={shlex.quote(d.get(\"known_hosts\", \"\"))}')
        print(f'_am_remote_cmd={shlex.quote(d.get(\"remote_cmd\", \"\"))}')
except (json.JSONDecodeError, KeyError) as e:
    print('_am_action=error')
    sys.exit(1)
")"

		if [[ "$_am_action" == "cd" ]]; then
			builtin cd "$_am_path" 2>/dev/null
			return 0
		elif [[ "$_am_action" == "ssh" ]]; then
			echo "✨🌟🪄🔮🌀🔮🪄🌟✨"
			# Run SSH directly — raw terminal, no capture.
			# Do NOT use 'exec' (would replace shell, user loses session on exit).
			# Use remote_cmd from binary if available (has correct ai-user path),
			# otherwise construct from path (fallback for old binary)
			if [[ -z "$_am_remote_cmd" ]]; then
				local _am_escaped_path="${_am_path//\'/\'\\\'\'}"
				_am_remote_cmd="git config --global --add safe.directory '${_am_escaped_path}' 2>/dev/null || true; cd '${_am_escaped_path}' && exec bash -l"
			fi
			ssh -tt -i "$_am_ssh_key" \
				-o ConnectTimeout=10 \
				-o ConnectionAttempts=1 \
				-o ServerAliveInterval=5 \
				-o ServerAliveCountMax=3 \
				-o IdentitiesOnly=yes \
				-o UserKnownHostsFile="$_am_known_hosts" \
				-o StrictHostKeyChecking=accept-new \
				"$_am_user@localhost" \
				"$_am_remote_cmd"
			local _am_ssh_ret=$?
			if [[ $_am_ssh_ret -ne 0 ]]; then
				echo "[fail] ssh exited with code $_am_ssh_ret"
			fi
			return $_am_ssh_ret
		else
			echo "[fail] unknown action: $_am_action"
			return 1
		fi
	else
		# Non-cd commands: pass through directly (preserves raw terminal)
		"$_am_bin" "$@"
		return $?
	fi
}

# Override 'am' as a function
am() { _am "$@"; }
export -f am
export -f _am
)DELIM";

  return 0;
}

} // namespace ai_mirror::cli

// am-mount-watch: standalone mount health checker & auto-repair
//
// Discovers AI users by scanning all system users' $HOME/.am_status files.
// For each AI user, reads the persisted `mounts` array from .am_status and
// compares each entry against /proc/mounts. Stale mounts (source missing or
// target inaccessible) are force-unmounted.
//
// This approach (introspection via .am_status) replaces the old
// naming-convention and .ai-mirror.toml scanning approach:
//   - No dependency on hex-hash username format (the prefix conventionally
//   starts
//     with "i", but any user with .am_status is treated as an AI user)
//   - No scanning of main user home directories (which crashes under NFS
//     root_squash)
//   - Self-contained: mount_watch reads expected mounts directly from each AI
//     user's state file
//
// Exit codes:
//   0 — all mounts healthy
//   1 — one or more stale mounts were detected and cleaned
//   2 — unfixable issues remain after cleanup
//
// Usage: am-mount-watch [--verbose]

#include "ai_mirror/core/config.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace utils = ai_mirror::utils;
namespace core = ai_mirror::core;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Check if a path has a readable .am_status file.
/// Uses std::error_code overload to avoid throwing filesystem_error on NFS
/// root_squash.
static bool has_status_file(const fs::path &home_dir) {
  std::error_code ec;
  return fs::exists(home_dir / ".am_status", ec);
}

/// Read .am_status and return the mounts array.
/// Returns empty vector if the file is missing or unreadable (graceful
/// degradation).
static std::vector<core::MountInfo>
read_expected_mounts(const fs::path &home_dir) {
  auto info = core::UserManager::read_state(home_dir);
  if (!info)
    return {};
  return std::move(info->mounts);
}

/// Read /proc/mounts for mount entries under the given home directory.
/// This is used for the one-time fill when .am_status has no mounts field yet
/// (legacy AI users).
static std::vector<core::MountEntry>
read_proc_mounts_for_user(const fs::path &home_dir) {
  std::vector<core::MountEntry> entries;
  std::ifstream mounts("/proc/mounts");
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string device, mount_point, fs_type, options;
    iss >> device >> mount_point >> fs_type >> options;
    if (mount_point.find(home_dir.string()) == 0 &&
        mount_point.size() > home_dir.string().size()) {
      core::MountEntry me;
      me.source = device;
      me.target = mount_point;
      me.read_only = options.find("ro") != std::string::npos;
      me.active = true;
      entries.push_back(std::move(me));
    }
  }
  return entries;
}

/// Build MountInfo from MountEntry by stat-ing the source.
/// For virtual device names (beegfs_nodev, tmpfs, proc, etc.), source is not
/// a real path — skip source stat to avoid ::stat() on a pseudo-device name.
static core::MountInfo mount_entry_to_info(const core::MountEntry &me) {
  core::MountInfo mi;
  mi.source = me.source.string();
  mi.target = me.target.string();
  mi.read_only = me.read_only;
  // Virtual device names don't start with '/' (e.g. "beegfs_nodev", "tmpfs")
  bool is_virtual_device = me.source.empty() || me.source.string()[0] != '/';
  if (!is_virtual_device) {
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
  return mi;
}

/// Check if a mount target is still alive by stat(2) on the target.
/// A bind mount becomes stale when its source is deleted, making the target
/// inaccessible. We do NOT compare inode/device numbers (see health_check in
/// graft.cpp for details on BeeGFS false-positive avoidance).
static bool is_mount_alive(const fs::path &target) {
  struct stat st;
  return ::stat(target.c_str(), &st) == 0;
}

/// Check if a mount source still exists via stat(2).
static bool source_exists(const fs::path &source) {
  struct stat st;
  return ::stat(source.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    std::string arg(argv[i]);
    if (arg == "--verbose" || arg == "-v") {
      verbose = true;
    }
  }

  // Logger: file-based, no screen output (systemd service)
  auto logger = utils::get_logger();
  logger->set_level(spdlog::level::info);

  // Must be root for mount operations
  if (geteuid() != 0) {
    logger->error("am-mount-watch requires root privileges");
    return 2;
  }

  logger->info("am-mount-watch starting");

  // Load optional system config for prefix.
  // The mount_watch binary runs as root (systemd), so we look for the config
  // in the system-wide install location only. We no longer scan main user
  // home directories for .ai-mirror.toml (NFS root_squash crash risk).
  core::Config config;
  std::string prefix = "i";
  {
    fs::path system_cfg("/etc/ai-mirror/config.toml");
    std::error_code ec;
    if (fs::exists(system_cfg, ec)) {
      try {
        config = core::ConfigParser::load(system_cfg);
        prefix = config.user.prefix.empty() ? "i" : config.user.prefix;
        logger->info("Loaded system config from {}", system_cfg.string());
      } catch (const std::exception &e) {
        // [log-review] 降级为 warning: config load 失败但使用默认值继续运行
        logger->warn("Failed to load config from {}, using defaults: {}",
                     system_cfg.string(), e.what());
      }
    } else {
      logger->info("No system config found at {}, using defaults (prefix={})",
                   system_cfg.string(), prefix);
    }
  }

  // Discover AI users by scanning /etc/passwd for users with .am_status.
  // Any user with a readable $HOME/.am_status is treated as an AI user.
  // This replaces the old naming-convention + prefix approach.
  std::vector<std::pair<std::string, fs::path>> ai_users; // username, home_dir
  {
    setpwent();
    while (auto *pw = getpwent()) {
      std::string home(pw->pw_dir);
      if (home.empty() || home == "/" || home == "/root")
        continue;
      // Check for .am_status (NFS-safe: uses error_code)
      if (has_status_file(home)) {
        ai_users.emplace_back(std::string(pw->pw_name), fs::path(home));
      }
    }
    endpwent();
  }

  if (ai_users.empty()) {
    logger->info("No AI users found (no .am_status files found)");
    return 0;
  }
  logger->info("Found {} AI user(s) via .am_status", ai_users.size());

  int total_fixed = 0;
  int total_unfixable = 0;
  std::vector<std::string> unfixable_users;

  for (const auto &[username, home_dir] : ai_users) {
    if (verbose) {
      logger->info("Checking user: {} (home: {})", username, home_dir.string());
    }

    // Read expected mounts from .am_status
    auto expected_mounts = read_expected_mounts(home_dir);

    // [补充机制] Legacy AI user: .am_status exists but no mounts field yet.
    // Read actual mounts from /proc/mounts, stat sources, and write back.
    if (expected_mounts.empty()) {
      auto proc_mounts = read_proc_mounts_for_user(home_dir);
      if (!proc_mounts.empty()) {
        logger->info(
            "  user {}: .am_status has no mounts, filling {} from /proc/mounts",
            username, proc_mounts.size());

        // Convert MountEntry to MountInfo
        std::vector<core::MountInfo> filled;
        for (const auto &me : proc_mounts) {
          filled.push_back(mount_entry_to_info(me));
        }
        expected_mounts = std::move(filled);

        // Persist back to .am_status via update_state_mounts
        // (We use Graft.list_mounts which reads /proc/mounts — same result)
        if (!core::UserManager::update_state_mounts(username, home_dir,
                                                    prefix)) {
          logger->warn(
              "  user {}: failed to write initial mounts to .am_status",
              username);
        }
      } else {
        if (verbose) {
          logger->info("  user {}: no mounts in .am_status or /proc/mounts, "
                       "skipping",
                       username);
        }
        continue;
      }
    }

    std::vector<fs::path> dead_mounts;

    for (const auto &mi : expected_mounts) {
      fs::path target(mi.target);
      fs::path source(mi.source);

      // Virtual device names (beegfs_nodev, tmpfs, proc, etc.) are not
      // real paths — skip source stat and only check target accessibility.
      // This mirrors the logic in Graft::health_check() (graft.cpp).
      bool is_virtual_device = mi.source.empty() || mi.source[0] != '/';

      // Check if target is still mounted (check via stat on target)
      bool target_alive = is_mount_alive(target);

      if (!target_alive) {
        // Target stat failed — stale mount or target path removed
        if (verbose) {
          logger->info("  stale mount target: {} (source: {})", mi.target,
                       mi.source);
        }
        dead_mounts.push_back(target);
        continue;
      }

      // Source existence check
      // (We stat the source directly; on NFS this is safe without
      // root_squash because mount_watch runs as root.)
      // Skip for virtual devices (e.g. BeeGFS "beegfs_nodev" pseudo-device).
      if (!is_virtual_device && !source_exists(source)) {
        if (verbose) {
          logger->info("  stale mount (source missing): {} -> {}", mi.source,
                       mi.target);
        }
        dead_mounts.push_back(target);
        continue;
      }
    }

    if (dead_mounts.empty()) {
      if (verbose) {
        logger->info("  user {}: all {} mount(s) healthy", username,
                     expected_mounts.size());
      }
      continue;
    }

    // Force-unmount stale mounts
    logger->info("  user {}: cleaning {} stale mount(s)", username,
                 dead_mounts.size());

    core::Graft graft(prefix);
    int cleaned = graft.force_cleanup(dead_mounts);
    total_fixed += cleaned;

    if (cleaned < static_cast<int>(dead_mounts.size())) {
      total_unfixable += static_cast<int>(dead_mounts.size()) - cleaned;
      unfixable_users.push_back(username);
      logger->warn("  user {}: {} mount(s) could not be cleaned", username,
                   dead_mounts.size() - cleaned);
    }
  }

  // Summary
  if (total_fixed > 0) {
    logger->info("Cleaned {} stale mount(s)", total_fixed);
  }
  if (total_unfixable > 0) {
    logger->warn("{} mount(s) remain unfixable for {} user(s)", total_unfixable,
                 unfixable_users.size());
  }
  logger->info("am-mount-watch finished");

  if (total_unfixable > 0) {
    return 2;
  }
  if (total_fixed > 0) {
    return 1;
  }
  return 0;
}

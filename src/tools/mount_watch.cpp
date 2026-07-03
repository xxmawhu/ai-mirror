// am-mount-watch: standalone mount health checker & auto-repair
//
// Discovers AI users by scanning all system users' $HOME/.am_status files.
// For each AI user, reads the persisted `mounts` array from .am_status and
// compares each entry against /proc/mounts. Stale mounts (source missing or
// target inaccessible) are reported (NOT unmounted).
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
//   2 — (reserved)
//
// Usage: am-mount-watch [--verbose]

#include "ai_mirror/core/config.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/core/vfs_util.hpp"
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
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace utils = ai_mirror::utils;
namespace core = ai_mirror::core;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Safely remount a stale bind mount.
/// 1. Lazy umount (detaches from namespace, keeps alive for open fds)
/// 2. Bind mount with current source
/// Returns true on success.
///
/// Safe under all conditions:
/// - Lazy umount never blocks and never fails on stale mounts (kernel drops
///   the mount reference, old inode kept alive for processes with open fds)
/// - New bind mount creates a fresh mapping to the current source inode
/// - Processes with old fds continue accessing old data (standard Linux
///   lazy-umount semantics, no data-loss risk)
static bool safe_remount(const fs::path &source, const fs::path &target) {
  auto logger = utils::get_logger();
  // Step 1: Lazy umount — detach stale mount from namespace.
  // Even if this fails (e.g. mount already gone), we still try bind.
  auto umount = utils::exec_safe({"umount", "-l", target.string()});
  if (umount.exit_code != 0) {
    // [防御] umount -l 失败可能是 mount 已不存在，不影响后续 bind
    logger->warn("  lazy umount failed (mount may already be gone): {}",
                 umount.stderr_output);
  }

  // Step 2: Bind mount with current source (creates new kernel mount)
  auto bind = utils::exec_safe(
      {"mount", "--bind", source.string(), target.string()});
  if (bind.exit_code != 0) {
    logger->error("  remount FAILED: {} -> {} - {}", source.string(),
                  target.string(), bind.stderr_output);
    return false;
  }

  return true;
}

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

/// Read /proc/self/mountinfo for mount entries under the given home directory.
/// Unlike /proc/mounts, mountinfo preserves the real source path for bind
/// mounts even on virtual filesystems like BeeGFS (where /proc/mounts shows
/// only "beegfs_nodev" with no path).
///
/// mountinfo format (space-separated):
///   id parent_id major:minor root target options... - fstype source super_opts
///
/// For bind mounts (root != "/"), the real source path is:
///   parent_mount_target + root_field
/// This works for ALL filesystem types:
///   - ext4:  parent_target("/") + root("/home/x/dir") = "/home/x/dir"  U2713
///   - beegfs: parent_target("/mnt/b") + root("/usr/x/lib") =
///   "/mnt/b/usr/x/lib"  U2713
///   - tmpfs: parent_target("/run") + root("/user/1000") = "/run/user/1000"
///   U2713
static std::vector<core::MountEntry>
read_mountinfo_for_user(const fs::path &home_dir) {
  std::vector<core::MountEntry> entries;
  std::ifstream mi("/proc/self/mountinfo");
  if (!mi)
    return entries;

  // First pass: build map of mount_id U2192 mount_target for parent lookups
  std::unordered_map<int, fs::path> parent_targets;
  // Collect bind mounts under home_dir for second pass
  struct BindMountInfo {
    int parent_id;
    fs::path root;
    fs::path target;
    std::string fstype;
    bool read_only;
  };
  std::vector<BindMountInfo> bind_mounts;

  std::string line;
  while (std::getline(mi, line)) {
    std::istringstream iss(line);
    int id, parent_id;
    std::string dev, root, target, options, fstype, source;
    // Read fixed-position fields: id, parent, major:minor, root,
    // mount_point, options
    if (!(iss >> id >> parent_id >> dev >> root >> target >> options))
      continue;

    // Skip optional fields until the separator '-'
    std::string sep;
    while (iss >> sep && sep != "-") {
    }
    if (iss.fail())
      continue;

    // Read fstype (after '-') and mount source
    if (!(iss >> fstype >> source))
      continue;

    parent_targets[id] = target;

    // Skip non-bind mounts (root == "/" means full filesystem mount)
    if (root == "/")
      continue;

    // Check if mount is under the target home directory
    if (target.find(home_dir.string()) == 0 &&
        target.size() > home_dir.string().size()) {
      bind_mounts.push_back({parent_id, root, target, fstype,
                             options.find("ro") != std::string::npos});
    }
  }

  // Second pass: construct absolute source paths
  for (const auto &bm : bind_mounts) {
    core::MountEntry me;
    // For bind mounts, reconstruct the absolute source path:
    //   absolute_source = parent_mount_target + root_field
    //
    // ⚠️ mountinfo root field is absolute (starts with '/'), e.g.,
    //    "/usr/maxx/.bashrc".  Using fs::path::operator/ would REPLACE
    //    the parent target with root (since root is absolute), giving the
    //    wrong result.  String concatenation preserves both parts:
    //      parent_target("/mnt/beegfs_data") + root("/usr/maxx/.bashrc")
    //      = "/mnt/beegfs_data/usr/maxx/.bashrc"  ✓
    auto parent_it = parent_targets.find(bm.parent_id);
    if (parent_it != parent_targets.end() && !bm.root.empty()) {
      // String concatenation to avoid fs::path::operator/ replacement bug
      // when root starts with '/'
      std::string parent_str = parent_it->second.string();
      if (!bm.root.empty() && bm.root.string()[0] == '/') {
        me.source = fs::path(parent_str + bm.root.string());
      } else {
        me.source = parent_it->second / bm.root;
      }
    } else {
      // Fallback: use target as source (legacy behavior)
      me.source = bm.target;
    }

    me.target = bm.target;
    me.fstype = bm.fstype;
    me.read_only = bm.read_only;
    me.active = true;
    entries.push_back(std::move(me));
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
  mi.fstype = me.fstype;
  mi.read_only = me.read_only;
  // Virtual filesystems (proc, tmpfs, beegfs_nodev, etc.) have no real
  // device backing.  Uses fstype when available, falls back to source
  // heuristic for backward compat.
  if (!ai_mirror::core::is_virtual_source(mi.source, mi.fstype)) {
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

  for (const auto &[username, home_dir] : ai_users) {
    if (verbose) {
      logger->info("Checking user: {} (home: {})", username, home_dir.string());
    }

    // Read expected mounts from .am_status
    auto expected_mounts = read_expected_mounts(home_dir);

    // [补充机制] Legacy AI user: .am_status exists but no mounts field yet.
    // Read actual mounts from /proc/mounts, stat sources, and write back.
    if (expected_mounts.empty()) {
      auto proc_mounts = read_mountinfo_for_user(home_dir);
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

    // Pre-read /proc/self/mountinfo once for the whole user, so we can
    // fall back to mountinfo-based source recovery when .am_status has
    // stale source paths (e.g., legacy files written without fstype or
    // before mountinfo-based path resolution).
    std::unordered_map<std::string, std::string> mountinfo_sources;
    {
      std::ifstream mi("/proc/self/mountinfo");
      if (mi) {
        std::string line;
        // First pass: collect parent mount targets (id -> target)
        std::unordered_map<int, std::string> parent_targets;
        struct BindMount {
          int parent_id;
          std::string root;
          std::string target;
        };
        std::vector<BindMount> candidates;
        while (std::getline(mi, line)) {
          std::istringstream iss(line);
          int id, parent_id;
          std::string dev, root, target, options, sep, fstype, source;
          // Read fixed-position fields up to options (field 6)
          if (!(iss >> id >> parent_id >> dev >> root >> target >> options))
            continue;
          // Skip optional fields (e.g., shared:1, master:5) until separator '-'
          while (iss >> sep && sep != "-") {
          }
          if (iss.fail())
            continue;
          // Read fstype and mount source after '-'
          if (!(iss >> fstype >> source))
            continue;
          parent_targets[id] = target;
          if (root != "/" && target.find(home_dir.string()) == 0 &&
              target.size() > home_dir.string().size()) {
            candidates.push_back({parent_id, root, target});
          }
        }
        // Second pass: construct absolute source paths
        for (const auto &c : candidates) {
          auto pit = parent_targets.find(c.parent_id);
          if (pit != parent_targets.end() && !c.root.empty() &&
              c.root[0] == '/') {
            mountinfo_sources[c.target] = pit->second + c.root;
          }
        }
      }
    }

    for (const auto &mi : expected_mounts) {
      fs::path target(mi.target);
      fs::path source(mi.source);

      // ================================================================
      // Layer 1: Target liveness check (ALL mounts, all filesystems)
      // ================================================================
      // A bind mount becomes stale when its source file is atomically
      // replaced (tmp+mv). The old source inode is unlinked and becomes
      // inaccessible.  stat(target) returns ENOENT — the only universal
      // stale-mount signal across ALL filesystem types (ext4, BeeGFS, NFS).
      //
      // We MUST check this for every mount, even those with "virtual"
      // fstype labels (BeeGFS), because the target is always a real path
      // on a real filesystem and stat() on it always works.
      bool is_virtual_source_path =
          ai_mirror::core::is_virtual_source(mi.source, mi.fstype);

      if (!is_mount_alive(target)) {
        // Target is dead — try safe remount via mountinfo
        auto mit = mountinfo_sources.find(mi.target);
        if (mit != mountinfo_sources.end() &&
            source_exists(mit->second)) {
          // mountinfo has correct source AND it exists → safe to remount
          logger->warn("  stale mount target (ENOENT): {}", mi.target);
          if (safe_remount(mit->second, target)) {
            logger->info("  remounted: {} -> {}", mit->second, mi.target);
            // Persist corrected state
            core::UserManager::update_state_mounts(username, home_dir,
                                                   prefix);
          }
        } else {
          logger->warn("  stale mount target, cannot recover: {} (src: {})",
                       mi.target, mi.source);
        }
        continue;
      }

      // ================================================================
      // Layer 2: Inode comparison (non-virtual FS only)
      // ================================================================
      // On ext4/xfs/btrfs, a healthy bind mount preserves the source's
      // inode at the target.  If source and target inodes differ, the
      // source file was atomically replaced and the mount points to
      // stale (unlinked) data.
      //
      // On BeeGFS (is_virtual_source_path=true), bind mounts do NOT
      // preserve inode — inode comparison would false-positive on every
      // healthy mount.  We skip inode check for virtual fstypes and rely
      // on Layer 1 (target ENOENT) instead.
      //
      // On NFS, inode numbers are reused across file lifetime so
      // comparison is unreliable.  NFS is classified as non-virtual
      // (fstype=nfs4, not in virtual_fstypes set).  We still attempt
      // inode comparison for NFS but it will rarely trigger (NFS inode
      // reuse would require the same inode to be reassigned, which is
      // practically unlikely within a 5-minute window).
      if (!is_virtual_source_path) {
        struct stat src_st, tgt_st;
        bool src_ok = (::stat(source.c_str(), &src_st) == 0);
        bool tgt_ok = (::stat(target.c_str(), &tgt_st) == 0);

        if (src_ok && tgt_ok &&
            (src_st.st_ino != tgt_st.st_ino ||
             src_st.st_dev != tgt_st.st_dev)) {
          // Inode mismatch: source was atomically replaced
          logger->warn(
              "  inode mismatch src={}/{} tgt={}/{} — stale data",
              src_st.st_dev, src_st.st_ino, tgt_st.st_dev, tgt_st.st_ino);
          if (safe_remount(source, target)) {
            logger->info("  remounted: {} -> {}", mi.source, mi.target);
            core::UserManager::update_state_mounts(username, home_dir,
                                                   prefix);
          }
          continue;
        }
      }

      // ================================================================
      // Layer 3: Source existence (virtual FS skip, real FS check)
      // ================================================================
      // Skip source stat for virtual fstypes (proc, tmpfs, beegfs, etc.)
      // to avoid stat() on a pseudo-device name that would always fail
      // or require a metadata server RPC on distributed filesystems.
      if (is_virtual_source_path) {
        continue;
      }

      if (!source_exists(source)) {
        // Source missing — try to recover from mountinfo
        auto mit = mountinfo_sources.find(mi.target);
        if (mit != mountinfo_sources.end() &&
            source_exists(mit->second)) {
          // mountinfo has correct path → persist it
          logger->warn("  stale source in .am_status: {} -> {} (correct: {})",
                       mi.source, mi.target, mit->second);
          core::UserManager::update_state_mounts(username, home_dir,
                                                 prefix);
        } else {
          logger->warn("  source missing, cannot recover: {} -> {}",
                       mi.source, mi.target);
          continue;
        }
      }
    }

    if (verbose) {
      logger->info("  user {}: {} mount(s) healthy", username,
                   expected_mounts.size());
    }
    continue;
  }

  logger->info("am-mount-watch finished (no unmounts performed)");
  return 0;
}

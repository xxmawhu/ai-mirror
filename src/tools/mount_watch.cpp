// am-mount-watch: standalone mount health checker & auto-repair
//
// Scans all AI users' bind mounts, detects stale mounts (source missing or
// target inaccessible), force-unmounts them, and reports results.
// Designed to run periodically via systemd timer.
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
#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <pwd.h>
#include <string>
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

/// Check if a string is a 6-character hex hash suffix.
static bool is_hex_suffix(const std::string &s) {
  if (s.size() != 6)
    return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
  }
  return true;
}

/// Find all AI users on the system.
///
/// An AI username follows the pattern: {prefix}{main_user}_{6-char-hex-hash}.
/// e.g. "imaxx_a1b2c3", "iitest_a1b2c3", "ialice_0f1e2d".
///
/// The robust match is:
///   1. Starts with prefix
///   2. Has a last underscore followed by exactly 6 hex characters
///   3. At least one character exists between prefix and the underscore
///
/// This correctly handles main users whose name starts with the prefix
/// (e.g. main user "itest" → AI user "iitest_a1b2c3").
static std::vector<std::string> list_ai_users(const std::string &prefix) {
  std::vector<std::string> users;
  setpwent();
  while (auto *pw = getpwent()) {
    std::string name(pw->pw_name);

    // Must start with prefix
    if (name.size() <= prefix.size() ||
        name.compare(0, prefix.size(), prefix) != 0)
      continue;

    // Must have a main_user segment (at least 1 char) + underscore + 6 hex
    // hash Minimum total: prefix + 1 + 1 + 6
    if (name.size() < prefix.size() + 8)
      continue;

    // Find last underscore
    auto last_us = name.rfind('_');
    if (last_us == std::string::npos)
      continue;

    // There must be at least 1 char between prefix end and the underscore
    if (last_us <= prefix.size())
      continue;

    // The suffix after last underscore must be 6 hex chars
    std::string suffix = name.substr(last_us + 1);
    if (!is_hex_suffix(suffix))
      continue;

    users.push_back(std::move(name));
  }
  endpwent();
  return users;
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

  // Load config from standard locations.
  // The mount_watch binary runs as root (systemd), so we look for the config
  // in common install locations rather than in the dev/build tree.
  core::Config config;
  std::string prefix = "i";
  {
    // Search order:
    //   1. /etc/ai-mirror/config.toml       (system-wide install)
    //   2. /home/*/.ai-mirror.toml          (main user home, try all)
    //   3. Default config (use hardcoded defaults)
    fs::path config_path;

    // Check /etc/ai-mirror/config.toml (system install)
    fs::path system_cfg("/etc/ai-mirror/config.toml");
    if (fs::exists(system_cfg)) {
      config_path = system_cfg;
      logger->info("Loading config from {}", system_cfg.string());
    } else {
      // Try to find config in any main user's home
      // Scan /home for .ai-mirror.toml files
      // (systemd runs as root, utils::get_effective_home() would give /root)
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator("/home", ec)) {
        if (!entry.is_directory())
          continue;
        fs::path user_cfg = entry.path() / ".ai-mirror.toml";
        if (fs::exists(user_cfg)) {
          config_path = user_cfg;
          logger->info("Loading config from {}", user_cfg.string());
          break;
        }
      }
      // Also check /mnt/beegfs_data/usr/*/ (for BeeGFS home)
      if (config_path.empty()) {
        std::error_code be_ec;
        for (const auto &entry :
             fs::directory_iterator("/mnt/beegfs_data/usr", be_ec)) {
          if (!entry.is_directory())
            continue;
          fs::path user_cfg = entry.path() / ".ai-mirror.toml";
          if (fs::exists(user_cfg)) {
            config_path = user_cfg;
            logger->info("Loading config from {}", user_cfg.string());
            break;
          }
        }
      }
    }

    if (!config_path.empty()) {
      try {
        config = core::ConfigParser::load(config_path);
        prefix = config.user.prefix.empty() ? "i" : config.user.prefix;
        logger->info("Loaded config: prefix={}, mount_paths={}", prefix,
                     config.mount.paths.size());
      } catch (const std::exception &e) {
        logger->warn("Failed to load config from {}, using defaults: {}",
                     config_path.string(), e.what());
      }
    } else {
      logger->info("No config found, using defaults (prefix={})", prefix);
    }
  }

  // Scan AI users
  auto ai_users = list_ai_users(prefix);
  if (ai_users.empty()) {
    logger->info("No AI users found (prefix={})", prefix);
    return 0;
  }
  logger->info("Found {} AI user(s)", ai_users.size());

  int total_fixed = 0;
  int total_unfixable = 0;
  std::vector<std::string> unfixable_users;

  for (const auto &username : ai_users) {
    if (verbose) {
      logger->info("Checking user: {}", username);
    }

    core::Graft graft(prefix);
    auto issues = graft.health_check();

    // Filter issues for this user
    std::vector<fs::path> dead_mounts;
    std::string user_home = ai_mirror::utils::get_home_dir(username);
    if (user_home.empty()) {
      logger->warn("Cannot determine home for user {}, skipping", username);
      continue;
    }

    for (const auto &m : issues) {
      if (m.target.string().find(user_home) == 0) {
        dead_mounts.push_back(m.target);
        if (verbose) {
          logger->info("  stale mount: {} -> {}", m.target.string(),
                       m.source.string());
        }
      }
    }

    if (dead_mounts.empty()) {
      if (verbose) {
        logger->info("  user {}: all mounts healthy", username);
      }
      continue;
    }

    // Force-unmount stale mounts
    logger->info("  user {}: cleaning {} stale mount(s)", username,
                 dead_mounts.size());
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

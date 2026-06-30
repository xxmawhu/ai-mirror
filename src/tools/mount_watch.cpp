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

/// Find all AI users (names starting with prefix_) on the system.
static std::vector<std::string> list_ai_users(const std::string &prefix) {
  std::vector<std::string> users;
  setpwent();
  while (auto *pw = getpwent()) {
    std::string name(pw->pw_name);
    // Match: starts with prefix + at least one char after underscore
    // e.g. "imaxx_a1b2c3"
    if (name.size() > prefix.size() + 1 &&
        name.compare(0, prefix.size(), prefix) == 0 &&
        name[prefix.size()] == '_') {
      users.push_back(std::move(name));
    }
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

  // Load config (default path: /etc/ai-mirror/config.toml)
  fs::path config_path = fs::path("/etc/ai-mirror/config.toml");
  if (!fs::exists(config_path)) {
    // Fallback: try project config
    config_path = fs::path(
        "/mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/etc/config.toml");
  }
  if (!fs::exists(config_path)) {
    logger->error("Config not found at /etc/ai-mirror/config.toml");
    return 2;
  }

  core::Config config;
  try {
    config = core::ConfigParser::load(config_path);
  } catch (const std::exception &e) {
    logger->error("Failed to load config: {}", e.what());
    return 2;
  }
  std::string prefix = config.user.prefix.empty() ? "i" : config.user.prefix;

  // Build mount source paths from config
  std::vector<fs::path> mount_sources;
  for (const auto &mp : config.mount.paths) {
    auto resolved = core::PathResolver::resolve(mp.string());
    if (resolved && fs::exists(*resolved)) {
      mount_sources.push_back(*resolved);
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

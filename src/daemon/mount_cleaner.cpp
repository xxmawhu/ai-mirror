#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"
#include <algorithm>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace ai_mirror::daemon {

namespace {
bool is_path_under(const std::string &path, const std::string &prefix) {
  if (path.find(prefix) != 0)
    return false;
  return path.length() == prefix.length() || path[prefix.length()] == '/';
}

bool is_valid_device_path(const std::string &device) {
  if (device.empty() || device[0] != '/')
    return false;
  if (device.find("..") != std::string::npos)
    return false;
  return true;
}
} // namespace

MountCleaner::MountCleaner(const std::string &user_prefix)
    : prefix_(user_prefix) {}

std::vector<fs::path> MountCleaner::find_stale_mounts() {
  std::vector<fs::path> stale;
  std::ifstream mounts("/proc/mounts");
  std::string line;

  // Collect ai-user home directories with validate_username check
  // (same logic as Graft::parse_mount_table for consistency)
  std::vector<std::string> ai_homes;
  setpwent();
  while (auto *pw = getpwent()) {
    std::string name(pw->pw_name);
    if (name.length() > prefix_.length() &&
        name.substr(0, prefix_.length()) == prefix_ &&
        utils::validate_username(name) &&
        std::string(pw->pw_dir).find('/') == 0) {
      ai_homes.push_back(pw->pw_dir);
    }
  }
  endpwent();

  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string device, mount_point;
    iss >> device >> mount_point;

    // Check if mount_point is under an ai-user home directory
    bool is_ai_user_mount = false;
    for (const auto &home : ai_homes) {
      if (mount_point.find(home) == 0) {
        std::string rest = mount_point.substr(home.size());
        if (rest.empty() || rest[0] == '/') {
          is_ai_user_mount = true;
          break;
        }
      }
    }

    if (is_ai_user_mount) {
      if (!is_valid_device_path(device)) {
        utils::get_logger()->warn("Invalid device path in mount entry: {}",
                                  device);
        stale.push_back(fs::path(mount_point));
        continue;
      }
      struct stat st;
      if (lstat(device.c_str(), &st) != 0) {
        stale.push_back(fs::path(mount_point));
        utils::get_logger()->warn("Stale mount found (device lstat failed): {}",
                                  mount_point);
      }
    }
  }

  return stale;
}

int MountCleaner::force_cleanup(const std::vector<fs::path> &mounts) {
  std::vector<std::string> ai_homes;
  setpwent();
  while (auto *pw = getpwent()) {
    std::string name(pw->pw_name);
    if (name.length() > prefix_.length() &&
        name.substr(0, prefix_.length()) == prefix_ &&
        utils::validate_username(name) &&
        std::string(pw->pw_dir).find('/') == 0) {
      ai_homes.push_back(pw->pw_dir);
    }
  }
  endpwent();

  int cleaned = 0;
  for (const auto &m : mounts) {
    std::string ms = m.string();

    bool is_ai_user_mount = false;
    for (const auto &home : ai_homes) {
      if (ms.find(home) == 0 &&
          (ms.length() == home.length() || ms[home.length()] == '/')) {
        is_ai_user_mount = true;
        break;
      }
    }
    if (!is_ai_user_mount) {
      utils::get_logger()->warn("Skipping non-ai-user mount in cleanup: {}",
                                ms);
      continue;
    }

    auto result = utils::exec_safe({"umount", "-l", ms});
    if (result.exit_code == 0) {
      cleaned++;
      utils::get_logger()->debug("Lazy unmounted: {}", ms);
    } else {
      utils::get_logger()->error("Failed to unmount {}: {}", ms,
                                 result.stderr_output);
    }
  }
  return cleaned;
}

int MountCleaner::cleanup_for_user(const std::string &username) {
  if (!utils::validate_username(username))
    return 0;

  std::vector<fs::path> user_mounts;

  std::ifstream mounts("/proc/mounts");
  std::string line;
  std::string user_home = utils::get_home_dir(username);
  if (user_home.empty()) {
    utils::get_logger()->warn("Cannot determine home for user {}", username);
    return 0;
  }

  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string device, mount_point;
    iss >> device >> mount_point;

    if (is_path_under(mount_point, user_home)) {
      user_mounts.push_back(fs::path(mount_point));
    }
  }

  return force_cleanup(user_mounts);
}

} // namespace ai_mirror::daemon

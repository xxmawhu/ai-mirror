#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/unique_fd.hpp"
#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <map>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>

namespace ai_mirror::core {

using security::safe_create_directories;

// Helper: chown all directories in path chain from target_parent up to (but not
// including) boundary_dir Only chown directories that don't already have the
// target ownership
static void chown_path_chain(const fs::path &target_parent,
                             const fs::path &boundary_dir, uid_t owner_uid,
                             gid_t owner_gid) {
  if (owner_uid == 0 && owner_gid == 0)
    return;

  fs::path abs_boundary = fs::weakly_canonical(boundary_dir);

  // Walk from target_parent up, collecting paths to chown
  // Stop at boundary_dir (do not chown boundary_dir itself or anything above
  // it)
  std::vector<fs::path> paths_to_chown;
  fs::path p = target_parent;
  while (!p.empty() && p != "/") {
    if (fs::weakly_canonical(p) == abs_boundary)
      break;
    struct stat st;
    if (stat(p.c_str(), &st) == 0) {
      // Only add if ownership differs
      if (st.st_uid != owner_uid || st.st_gid != owner_gid) {
        paths_to_chown.push_back(p);
      }
    }
    p = p.parent_path();
  }

  // Chown from top (closest to boundary) down to target
  // This ensures parent directories are chowned before children
  for (auto it = paths_to_chown.rbegin(); it != paths_to_chown.rend(); ++it) {
    if (chown(it->c_str(), owner_uid, owner_gid) == 0) {
      utils::get_logger()->info("chown_path_chain: {} -> uid:{} gid:{}",
                                it->string(), owner_uid, owner_gid);
    } else {
      utils::get_logger()->warn("chown_path_chain: chown {} failed: {}",
                                it->string(), strerror(errno));
    }
  }
}

Graft::Graft(const std::string &user_prefix) : prefix_(user_prefix) {}

const std::vector<MountEntry> &Graft::get_mount_cache() const {
  auto now = std::chrono::steady_clock::now();
  if (cache_valid_ && (now - cache_time_) < cache_ttl_) {
    return mount_cache_;
  }
  mount_cache_ = parse_mount_table();
  cache_time_ = now;
  cache_valid_ = true;
  return mount_cache_;
}

void Graft::invalidate_cache() {
  cache_valid_ = false;
  mount_cache_.clear();
}

bool Graft::execute_mount(const fs::path &source, const fs::path &target,
                          bool read_only, uid_t owner_uid, gid_t owner_gid,
                          const fs::path &home_dir) {
  std::error_code ec;
  // Determine boundary: stop chown at home_dir, never chown above ai-user home
  fs::path boundary =
      home_dir.empty() ? target.parent_path().parent_path() : home_dir;

  if (!fs::exists(target, ec)) {
    if (fs::is_regular_file(source)) {
      fs::path parent = target.parent_path();
      if (!parent.empty() && !fs::exists(parent, ec)) {
        if (!safe_create_directories(parent)) {
          utils::get_logger()->error(
              "execute_mount: failed to create parent dir for {}",
              target.string());
          return false;
        }
        // Chown parent directories to ai-user, bounded by home_dir
        if (owner_uid != 0 || owner_gid != 0) {
          chown_path_chain(parent, boundary, owner_uid, owner_gid);
        }
      }
      utils::unique_fd ufd(
          open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644));
      if (!ufd) {
        utils::get_logger()->error(
            "execute_mount: create target file failed: {} ({})",
            target.string(), strerror(errno));
        return false;
      }
      if (owner_uid != 0 || owner_gid != 0) {
        if (fchown(ufd.get(), owner_uid, owner_gid) != 0) {
          utils::get_logger()->warn(
              "execute_mount: fchown file failed: {} ({})", target.string(),
              strerror(errno));
        }
      }
      ufd.reset();
    } else {
      // Create target directory and all parents (e.g.,
      // /home/ai_user/.local/bin)
      if (!safe_create_directories(target)) {
        utils::get_logger()->error(
            "execute_mount: failed to create target dir {}", target.string());
        return false;
      }
      if (owner_uid != 0 || owner_gid != 0) {
        // Chown all intermediate directories first (e.g., .local/ when mounting
        // .local/bin)
        fs::path parent = target.parent_path();
        if (!parent.empty()) {
          chown_path_chain(parent, boundary, owner_uid, owner_gid);
        }
        // Then chown the final target directory
        utils::unique_fd dir_fd(
            open(target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
        if (dir_fd) {
          if (fchown(dir_fd.get(), owner_uid, owner_gid) != 0) {
            utils::get_logger()->warn(
                "execute_mount: fchown dir failed: {} ({})", target.string(),
                strerror(errno));
          }
        }
      }
    }
  } else if (owner_uid != 0 || owner_gid != 0) {
    // Target already exists — fix ownership of intermediate dirs and target
    // itself This handles the case where directories were created by root on
    // first run and am cd is called again (e.g., after a fix deployment)
    fs::path parent = target.parent_path();
    if (!parent.empty()) {
      chown_path_chain(parent, boundary, owner_uid, owner_gid);
    }
    // Fix target ownership too
    struct stat st;
    if (stat(target.c_str(), &st) == 0 &&
        (st.st_uid != owner_uid || st.st_gid != owner_gid)) {
      utils::unique_fd fd(
          open(target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
      if (fd) {
        if (fchown(fd.get(), owner_uid, owner_gid) != 0) {
          utils::get_logger()->warn(
              "execute_mount: fix ownership failed for existing {}: {}",
              target.string(), strerror(errno));
        } else {
          utils::get_logger()->info(
              "execute_mount: fixed ownership of existing {}", target.string());
        }
      }
    }
  }

  auto validation = security::validate_mount_paths(source, target);
  if (!validation.safe) {
    utils::get_logger()->error("Mount validation failed: {}",
                               validation.reason);
    return false;
  }

  if (read_only) {
    auto result = utils::exec_safe(
        {"mount", "--bind", "-o", "ro", source.string(), target.string()});
    if (result.exit_code == 0) {
      utils::get_logger()->info("Bind mounted {} -> {} (ro, single-step)",
                                source.string(), target.string());
      return true;
    }
    utils::get_logger()->warn("Single-step ro mount failed (kernel <5.12?), "
                              "falling back to two-step: {}",
                              result.stderr_output);
  }

  auto result =
      utils::exec_safe({"mount", "--bind", source.string(), target.string()});
  if (result.exit_code != 0) {
    if (result.stderr_output.find("busy") != std::string::npos ||
        result.stderr_output.find("EBUSY") != std::string::npos) {
      utils::get_logger()->info("Target already mounted (EBUSY): {}",
                                target.string());
      return true;
    }
    utils::get_logger()->error("mount --bind failed: {}", result.stderr_output);
    return false;
  }

  if (read_only) {
    auto remount_result =
        utils::exec_safe({"mount", "-o", "remount,bind,ro", target.string()});
    if (remount_result.exit_code != 0) {
      utils::get_logger()->warn("Failed to remount as read-only: {}",
                                remount_result.stderr_output);
    }
  }

  utils::get_logger()->info("Bind mounted {} -> {} (ro={})", source.string(),
                            target.string(), read_only);
  invalidate_cache();
  return true;
}

bool Graft::execute_umount(const fs::path &target, bool lazy) {
  std::vector<std::string> args;
  args.reserve(3);
  args.push_back("umount");
  if (lazy) {
    args.push_back("-l");
  }
  args.push_back(target.string());

  auto result = utils::exec_safe(args);
  if (result.exit_code != 0) {
    utils::get_logger()->error("umount failed for {}: {}", target.string(),
                               result.stderr_output);
    return false;
  }

  utils::get_logger()->info("Unmounted {} (lazy={})", target.string(), lazy);
  invalidate_cache();
  return true;
}

std::vector<MountEntry> Graft::parse_mount_table() const {
  std::vector<MountEntry> entries;
  std::ifstream mounts("/proc/mounts");
  std::string line;

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

  std::set<std::string> seen_targets;

  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string device, mount_point, fs_type, options;
    iss >> device >> mount_point >> fs_type >> options;

    if (seen_targets.count(mount_point))
      continue;

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
      MountEntry entry;
      entry.source = device;
      entry.target = mount_point;
      entry.read_only = options.find("ro") != std::string::npos;
      entry.active = true;
      entries.push_back(entry);
      seen_targets.insert(mount_point);
    }
  }

  return entries;
}

bool Graft::install_file_access(const fs::path &source, const fs::path &target,
                                const fs::path &home_dir) {
  auto logger = utils::get_logger();

  if (home_dir.empty()) {
    logger->error("install_file_access: home_dir is required");
    return false;
  }

  if (!fs::is_regular_file(source)) {
    logger->error("install_file_access: source is not a regular file: {}",
                  source.string());
    return false;
  }

  // --- Step 1: set source file permissions ---
  // Remove group/other write, preserve everything else (including +x bits).
  {
    struct stat st;
    if (stat(source.c_str(), &st) != 0) {
      logger->error("install_file_access: stat failed for {}: {}",
                    source.string(), strerror(errno));
      return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
      mode_t new_mode = st.st_mode & ~(S_IWGRP | S_IWOTH);
      if (chmod(source.c_str(), new_mode) != 0) {
        logger->error("install_file_access: chmod g-w,o-w failed for {}: {}",
                      source.string(), strerror(errno));
        return false;
      }
      logger->info("install_file_access: set {:o} on source {} (was {:o})",
                   new_mode & 07777, source.string(), st.st_mode & 07777);
    } else {
      logger->info("install_file_access: source {} already has no group/other "
                   "write (mode {:o}), skipping chmod",
                   source.string(), st.st_mode & 07777);
    }
  }

  // --- Step 2: create .am-mounts/ directory (root-owned) ---
  // The AI user's home dir is writable by them, so a symlink placed directly
  // there can be deleted. We use a root-owned subdirectory instead.
  // .am-mounts/ is created as root:ai_group with mode 0755. The AI user can
  // read/traverse but NOT write/delete inside it.
  struct stat home_st;
  gid_t ai_gid = 0;
  if (stat(home_dir.c_str(), &home_st) == 0) {
    ai_gid = home_st.st_gid;
  } else {
    logger->warn("install_file_access: cannot stat home_dir {}, "
                 "using gid=0: {}",
                 home_dir.string(), strerror(errno));
  }

  fs::path am_mounts = home_dir / ".am-mounts";
  bool mounts_dir_created = false;
  std::error_code ec;
  if (!fs::exists(am_mounts, ec)) {
    if (mkdir(am_mounts.c_str(), 0755) != 0) {
      logger->error("install_file_access: mkdir .am-mounts failed: {} ({})",
                    am_mounts.string(), strerror(errno));
      return false;
    }
    mounts_dir_created = true;
  }

  // Fix ownership: root:ai_group, even if dir already existed
  if (ai_gid != 0) {
    if (chown(am_mounts.c_str(), 0, ai_gid) != 0) {
      logger->warn("install_file_access: chown root:{} on {} failed: {}",
                   ai_gid, am_mounts.string(), strerror(errno));
    }
  } else {
    if (chown(am_mounts.c_str(), 0, 0) != 0) {
      logger->warn("install_file_access: chown root:root on {} failed: {}",
                   am_mounts.string(), strerror(errno));
    }
  }
  // Ensure correct mode (mkdir + umask might not give 755)
  chmod(am_mounts.c_str(), 0755);
  if (mounts_dir_created) {
    logger->info(
        "install_file_access: created .am-mounts/ at {} (root:{}, 755)",
        am_mounts.string(), ai_gid);
  }

  // --- Step 3: create symlink at .am-mounts/<basename> → source ---
  // This symlink is owned by root:root and is inside the root-owned
  // .am-mounts/ directory, so the AI user cannot delete or modify it.
  fs::path sym_path = am_mounts / source.filename();

  // Remove any stale entry at the symlink path
  std::error_code rm_ec;
  if (fs::exists(sym_path, rm_ec) ||
      fs::is_symlink(rm_ec ? sym_path : sym_path)) {
    fs::remove(sym_path, rm_ec);
  }

  if (symlink(source.c_str(), sym_path.c_str()) != 0) {
    logger->error("install_file_access: symlink failed: {} -> {} ({})",
                  sym_path.string(), source.string(), strerror(errno));
    return false;
  }
  logger->info("install_file_access: symlink created (protected): {} -> {}",
               sym_path.string(), source.string());

  // Set symlink ownership to root:root
  if (lchown(sym_path.c_str(), 0, 0) != 0) {
    logger->warn(
        "install_file_access: lchown root:root on symlink {} failed: {}",
        sym_path.string(), strerror(errno));
  }

  // --- Step 4: create user-level wrapper at original target path ---
  // The wrapper at `target` is a regular file owned by the AI user, so they
  // CAN delete it. But it's just a short sourcing script that loads the
  // real content from .am-mounts/. am update will recreate the wrapper.
  // This step is best-effort: if it fails, the symlink is still accessible
  // at .am-mounts/<basename>.
  {
    // Remove old bind mount / symlink / file at target
    if (fs::exists(target, ec) || fs::is_symlink(ec ? target : target)) {
      if (is_mounted_live(target)) {
        execute_umount(target, false);
      }
      fs::remove(target, rm_ec);
    }

    // Write a sourcing wrapper
    std::string wrapper_content =
        "# Managed by ai-mirror. Do not edit.\n"
        "# Real file is at .am-mounts/. Edit the source file instead.\n"
        "if [ -f \"$HOME/.am-mounts/" +
        source.filename().string() +
        "\" ]; then\n"
        "    . \"$HOME/.am-mounts/" +
        source.filename().string() +
        "\"\n"
        "fi\n";

    // Create parent directories
    fs::path parent = target.parent_path();
    if (!parent.empty() && !fs::exists(parent, ec)) {
      safe_create_directories(parent);
    }

    utils::unique_fd wfd(
        open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644));
    if (wfd) {
      ssize_t written =
          write(wfd.get(), wrapper_content.data(), wrapper_content.size());
      if (written == static_cast<ssize_t>(wrapper_content.size())) {
        logger->info("install_file_access: created sourcing wrapper at {}",
                     target.string());
      }
      wfd.reset();
    } else {
      // Wrapper creation is best-effort; the protected symlink is already in
      // place
      logger->warn("install_file_access: failed to create wrapper at {}: {}",
                   target.string(), strerror(errno));
    }
  }

  invalidate_cache();
  return true;
}

bool Graft::bind_mount(const fs::path &source, const fs::path &target,
                       bool read_only, uid_t owner_uid, gid_t owner_gid,
                       const fs::path &home_dir) {
  if (!security::validate_mount_source(source)) {
    utils::get_logger()->error("Mount source rejected (system path): {}",
                               source.string());
    return false;
  }

  auto pre_mount_source = security::safe_canonical(source);
  if (pre_mount_source.empty()) {
    utils::get_logger()->error("Mount source canonical resolution failed: {}",
                               source.string());
    return false;
  }

  if (is_mounted(target)) {
    utils::get_logger()->info(
        "Target already mounted, will still fix ownership if needed: {}",
        target.string());
  }

  // Double-check with live /proc/mounts to avoid cache staleness
  if (is_mounted_live(target)) {
    utils::get_logger()->info("Target already mounted (live check), will still "
                              "fix ownership if needed: {}",
                              target.string());
  }

  if (!security::validate_path_exists(source)) {
    utils::get_logger()->error(
        "Mount source path does not exist or is not regular/dir: {}",
        source.string());
    return false;
  }

  auto pre_exec_source = security::safe_canonical(source);
  if (pre_exec_source != pre_mount_source) {
    utils::get_logger()->error(
        "Mount source path changed between validation and execution: {}",
        source.string());
    return false;
  }

  // Redirect: regular files use symlink + permission setting instead of bind
  // mount. This avoids kernel mount overhead for individual files and allows
  // transparent access. Directories continue to use mount --bind.
  if (fs::is_regular_file(pre_exec_source)) {
    utils::get_logger()->info(
        "Source is a regular file, installing file access (symlink + chmod) "
        "instead of bind mount: {}",
        pre_exec_source.string());
    return install_file_access(pre_exec_source, target, home_dir);
  }

  return execute_mount(pre_exec_source, target, read_only, owner_uid, owner_gid,
                       home_dir);
}

bool Graft::unmount(const fs::path &target, bool lazy) {
  return execute_umount(target, lazy);
}

bool Graft::unmount_all(const std::string &username) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username for unmount_all: {}",
                               username);
    return false;
  }

  std::string prefix_check = prefix_ + utils::get_effective_username() + "_";
  if (username.length() <= prefix_check.length() ||
      username.substr(0, prefix_check.length()) != prefix_check) {
    utils::get_logger()->error("Refusing to unmount_all non-ai-mirror user: {}",
                               username);
    return false;
  }

  auto mounts = get_mount_cache();
  bool all_ok = true;

  std::string user_home = utils::get_home_dir(username);
  for (const auto &m : mounts) {
    if (!user_home.empty() && m.target.string().find(user_home) == 0) {
      if (!unmount(m.target, false)) {
        all_ok = false;
      }
    }
  }

  return all_ok;
}

std::vector<MountEntry> Graft::list_mounts(const std::string &username) const {
  auto all = get_mount_cache();
  std::string user_home = utils::get_home_dir(username);

  std::vector<MountEntry> user_mounts;
  std::copy_if(all.begin(), all.end(), std::back_inserter(user_mounts),
               [&](const MountEntry &m) {
                 return !user_home.empty() &&
                        m.target.string().find(user_home) == 0;
               });

  return user_mounts;
}

bool Graft::is_mounted(const fs::path &target) const {
  auto canon_target = security::safe_canonical(target);
  for (const auto &m : get_mount_cache()) {
    if (security::safe_canonical(m.target) == canon_target) {
      return true;
    }
  }
  return false;
}

// Live check: bypass cache and read /proc/mounts directly (for critical mount
// operations)
bool Graft::is_mounted_live(const fs::path &target) const {
  auto canon_target = security::safe_canonical(target);
  std::ifstream mounts("/proc/mounts");
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string device, mount_point, fs_type, options;
    iss >> device >> mount_point >> fs_type >> options;
    if (security::safe_canonical(fs::path(mount_point)) == canon_target) {
      return true;
    }
  }
  return false;
}

int Graft::cleanup_duplicate_mounts(const std::string &username) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error(
        "Invalid username for cleanup_duplicate_mounts: {}", username);
    return 0;
  }

  std::string user_home = utils::get_home_dir(username);
  if (user_home.empty()) {
    utils::get_logger()->warn("Cannot determine home for user {}", username);
    return 0;
  }

  std::ifstream mounts("/proc/mounts");
  if (!mounts.is_open()) {
    utils::get_logger()->error(
        "Cannot open /proc/mounts for duplicate cleanup");
    return 0;
  }
  std::string line;

  // Collect all mount entries for this user, preserving order.
  // Later entries in /proc/mounts are the "newer" stacked mounts.
  std::vector<std::pair<std::string, std::string>> user_entries;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string device, mount_point, fs_type, options;
    iss >> device >> mount_point >> fs_type >> options;

    if (mount_point.find(user_home) == 0 &&
        (mount_point.length() == user_home.length() ||
         mount_point[user_home.length()] == '/')) {
      user_entries.emplace_back(mount_point, line);
    }
  }

  // Group by target path to find duplicates
  std::map<std::string, std::vector<size_t>> target_groups;
  for (size_t i = 0; i < user_entries.size(); ++i) {
    target_groups[user_entries[i].first].push_back(i);
  }

  int cleaned = 0;
  for (const auto &[target, indices] : target_groups) {
    if (indices.size() <= 1)
      continue;

    // Linux mounts stack when same target is mounted multiple times.
    // Each umount -l pops one mount from the stack; calling N-1
    // times leaves exactly one mount (the newest/last entry).
    for (size_t j = 0; j < indices.size() - 1; ++j) {
      utils::get_logger()->info("Removing duplicate mount #{} of {} for {}: {}",
                                j + 1, indices.size(), username, target);
      auto result = utils::exec_safe({"umount", "-l", target});
      if (result.exit_code == 0) {
        cleaned++;
        utils::get_logger()->info("Lazy unmounted duplicate: {}", target);
      } else {
        utils::get_logger()->warn("Failed to umount duplicate {}: {}", target,
                                  result.stderr_output);
      }
    }
  }

  if (cleaned > 0) {
    invalidate_cache();
    utils::get_logger()->info("Cleaned up {} duplicate mount(s) for {}",
                              cleaned, username);
  }

  return cleaned;
}

bool Graft::ensure_group_exists(const std::string &groupname) {
  if (!utils::validate_username(groupname)) {
    utils::get_logger()->error("Invalid group name: {}", groupname);
    return false;
  }
  auto result = utils::exec_safe({"getent", "group", groupname});
  if (result.exit_code != 0) {
    result = utils::exec_safe({"groupadd", groupname});
    if (result.exit_code != 0) {
      utils::get_logger()->error("groupadd failed for {}: {}", groupname,
                                 result.stderr_output);
      return false;
    }
    utils::get_logger()->info("Created group: {}", groupname);
  }
  return true;
}

bool Graft::set_directory_group(const fs::path &path,
                                const std::string &groupname) {
  struct group *gr = getgrnam(groupname.c_str());
  if (!gr) {
    utils::get_logger()->error("set_directory_group: group '{}' not found",
                               groupname);
    return false;
  }

  struct passwd *pw = getpwnam(groupname.c_str());
  uid_t owner_uid = pw ? pw->pw_uid : static_cast<uid_t>(-1);

  utils::unique_fd ufd(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
  if (!ufd) {
    if (errno == ELOOP) {
      utils::get_logger()->error(
          "set_directory_group: path is a symlink, rejecting: {}",
          path.string());
      return false;
    }
    if (errno == ENOTDIR) {
      ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
      if (!ufd) {
        if (errno == ELOOP) {
          if (lchown(path.c_str(), owner_uid, gr->gr_gid) != 0) {
            utils::get_logger()->error(
                "set_directory_group: lchown failed for {}: {}", path.string(),
                strerror(errno));
            return false;
          }
          return true;
        }
        utils::get_logger()->error(
            "set_directory_group: open file failed for {}: {}", path.string(),
            strerror(errno));
        return false;
      }
    } else {
      utils::get_logger()->error("set_directory_group: open {} failed: {}",
                                 path.string(), strerror(errno));
      return false;
    }
  }

  int ret = fchown(ufd.get(), owner_uid, gr->gr_gid);
  ufd.reset();
  if (ret != 0) {
    utils::get_logger()->error("set_directory_group: fchown failed for {}: {}",
                               path.string(), strerror(errno));
    return false;
  }
  return true;
}

bool Graft::set_sgid(const fs::path &path) {
  utils::unique_fd ufd(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
  if (!ufd) {
    if (errno == ELOOP) {
      utils::get_logger()->error("set_sgid: path is a symlink, rejecting: {}",
                                 path.string());
      return false;
    }
    if (errno == ENOTDIR) {
      ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
      if (!ufd) {
        if (errno == ELOOP) {
          struct stat st;
          if (lstat(path.c_str(), &st) == 0) {
            mode_t new_mode = st.st_mode | S_ISGID;
            if (fchmodat(AT_FDCWD, path.c_str(), new_mode,
                         AT_SYMLINK_NOFOLLOW) != 0) {
              utils::get_logger()->error(
                  "set_sgid: fchmodat symlink failed for {}: {}", path.string(),
                  strerror(errno));
              return false;
            }
            return true;
          }
        }
        utils::get_logger()->error("set_sgid: open file failed for {}: {}",
                                   path.string(), strerror(errno));
        return false;
      }
    } else {
      utils::get_logger()->error("set_sgid: open {} failed: {}", path.string(),
                                 strerror(errno));
      return false;
    }
  }

  struct stat st;
  if (fstat(ufd.get(), &st) != 0) {
    ufd.reset();
    utils::get_logger()->error("set_sgid: fstat failed for {}: {}",
                               path.string(), strerror(errno));
    return false;
  }

  mode_t new_mode = st.st_mode | S_ISGID;
  int ret = fchmod(ufd.get(), new_mode);
  ufd.reset();
  if (ret != 0) {
    utils::get_logger()->error("set_sgid: fchmod failed for {}: {}",
                               path.string(), strerror(errno));
    return false;
  }
  return true;
}

bool Graft::grant_write_access(const fs::path &path,
                               const std::string &username) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username for write access: {}",
                               username);
    return false;
  }

  if (!security::validate_mount_source(path)) {
    utils::get_logger()->error(
        "Grant write path rejected (system directory): {}", path.string());
    return false;
  }

  if (!ensure_group_exists(username)) {
    return false;
  }

  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (!safe_create_directories(path)) {
      utils::get_logger()->error("Failed to create directory (safe): {}",
                                 path.string());
      return false;
    }
  }

  if (!set_directory_group(path, username)) {
    return false;
  }

  utils::unique_fd ufd(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
  if (!ufd) {
    if (errno == ELOOP) {
      utils::get_logger()->error(
          "grant_write_access: path is a symlink, rejecting: {}",
          path.string());
      return false;
    }
    if (errno == ENOTDIR) {
      ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
      if (!ufd) {
        if (errno == ELOOP) {
          utils::get_logger()->error(
              "grant_write_access: file path is symlink, rejecting: {}",
              path.string());
          return false;
        }
        utils::get_logger()->error(
            "grant_write_access: open file failed for {}: {}", path.string(),
            strerror(errno));
        return false;
      }
    } else {
      utils::get_logger()->error("grant_write_access: open {} failed: {}",
                                 path.string(), strerror(errno));
      return false;
    }
  }

  struct stat st;
  if (fstat(ufd.get(), &st) != 0) {
    ufd.reset();
    utils::get_logger()->error("grant_write_access: fstat failed for {}: {}",
                               path.string(), strerror(errno));
    return false;
  }

  mode_t new_mode = st.st_mode | (S_IRGRP | S_IWGRP | S_IXGRP);
  if (S_ISDIR(st.st_mode)) {
    new_mode |= S_IXGRP;
  }
  if (fchmod(ufd.get(), new_mode) != 0) {
    ufd.reset();
    utils::get_logger()->error("grant_write_access: fchmod failed for {}: {}",
                               path.string(), strerror(errno));
    return false;
  }
  ufd.reset();

  if (fs::is_directory(path)) {
    set_sgid(path);
  }

  utils::get_logger()->info("Granted group write access: {} -> group {}",
                            path.string(), username);
  return true;
}

bool Graft::revoke_write_access(const fs::path &path,
                                const std::string &username) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username for revoke: {}", username);
    return false;
  }

  utils::unique_fd ufd(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
  bool is_dir = true;
  if (!ufd) {
    if (errno == ELOOP) {
      utils::get_logger()->error(
          "revoke_write_access: path is a symlink, rejecting: {}",
          path.string());
      return false;
    }
    if (errno == ENOTDIR) {
      is_dir = false;
      ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
      if (!ufd) {
        if (errno == ELOOP) {
          utils::get_logger()->error(
              "revoke_write_access: file path is symlink, rejecting: {}",
              path.string());
          return false;
        }
        utils::get_logger()->warn(
            "revoke_write_access: open file failed for {}: {}", path.string(),
            strerror(errno));
      }
    } else {
      utils::get_logger()->warn("revoke_write_access: open {} failed: {}",
                                path.string(), strerror(errno));
    }
  }

  bool ok = true;

  if (ufd) {
    struct stat st;
    if (fstat(ufd.get(), &st) == 0) {
      mode_t new_mode = st.st_mode;
      if (is_dir) {
        new_mode &= ~S_ISGID;
      }
      new_mode &= ~(S_IRGRP | S_IWGRP | S_IXGRP);
      if (fchmod(ufd.get(), new_mode) != 0) {
        utils::get_logger()->warn(
            "revoke_write_access: fchmod failed for {}: {}", path.string(),
            strerror(errno));
        ok = false;
      }
    }
    ufd.reset();
  }

  auto gpasswd_result = utils::exec_safe({"gpasswd", "-d", username, username});
  if (gpasswd_result.exit_code != 0) {
    utils::get_logger()->warn(
        "gpasswd -d failed (user may not be in group): {}",
        gpasswd_result.stderr_output);
  }

  // Safety check: only delete group if it has no other members.  An attacker
  // could add themselves or another user to the ai-user's group before revoke,
  // causing groupdel to fail or remove a legitimate group used by others.
  // This prevents accidental DoS on shared groups.
  struct group *gr = getgrnam(username.c_str());
  if (!gr) {
    utils::get_logger()->info("Group '{}' does not exist, skipping groupdel",
                              username);
  } else {
    bool has_others = false;
    for (char **mem = gr->gr_mem; *mem != nullptr; ++mem) {
      if (std::string(*mem) != username) {
        has_others = true;
        break;
      }
    }
    if (has_others) {
      utils::get_logger()->warn("Group '{}' has other members besides '{}', "
                                "skipping groupdel to avoid DoS",
                                username, username);
    } else {
      utils::exec_safe({"groupdel", username});
    }
  }

  utils::get_logger()->info("Revoked write access: {} from group {}",
                            path.string(), username);
  return ok;
}

std::vector<MountEntry> Graft::health_check() const {
  auto mounts = get_mount_cache();
  std::vector<MountEntry> issues;

  for (const auto &m : mounts) {
    std::error_code ec;

    // For virtual device names (beegfs_nodev, tmpfs, proc, etc.),
    // check target existence since source is not a real path.
    // For real path sources, check source existence.
    bool is_virtual_device = !m.source.empty() && m.source.string()[0] != '/';

    if (is_virtual_device) {
      // Virtual device: check if target (mount point) exists and is accessible
      if (!fs::exists(m.target, ec)) {
        MountEntry broken = m;
        broken.active = false;
        issues.push_back(broken);
      }
    } else {
      // Real path source: check if source exists
      if (!fs::exists(m.source, ec)) {
        MountEntry broken = m;
        broken.active = false;
        issues.push_back(broken);
        continue;
      }

      // Stale mount detection.
      // Background: a bind mount becomes stale when its source is deleted.
      // The reliable, filesystem-agnostic signal is that stat() on the target
      // fails (target becomes inaccessible). We intentionally do NOT compare
      // inode/device numbers: on distributed filesystems such as BeeGFS, bind
      // mount targets do not preserve the source's inode, so an inode
      // comparison would flag every healthy BeeGFS mount as stale and trigger
      // spurious umount + mount-point removal (data-loss risk). See issue
      // 2026-06-23-2026-06-23-09-10-42-810-info-Stale-mount.md for the
      // false-positive that destroyed /mnt/beegfs_data/.../.local/bin.
      struct stat target_st;
      bool target_stat_ok = (stat(m.target.c_str(), &target_st) == 0);

      if (!target_stat_ok) {
        // Target inaccessible — truly stale mount (all filesystems).
        MountEntry broken = m;
        broken.active = false;
        issues.push_back(broken);
      }
    }
  }

  return issues;
}

int Graft::force_cleanup(const std::vector<fs::path> &dead_mounts) {
  int cleaned = 0;
  for (const auto &m : dead_mounts) {
    if (unmount(m, true)) {
      cleaned++;
    }
  }
  return cleaned;
}

} // namespace ai_mirror::core

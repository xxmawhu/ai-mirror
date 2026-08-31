#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"
#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace ai_mirror::security {
namespace {

// FHS system directories - all paths under these are forbidden.
// Covers privileged (/etc, /root, /boot) and shared (/tmp, /opt, /srv, /media).
// /mnt is intentionally excluded: BeeGFS and other shared filesystems mount
// there. /lost+found is filesystem-specific recovery directory (ext4 etc.).
//
// Returned as a static accessor (not a file-scope var) so that both
// validate_path_allowed() and matched_system_dir() share the SAME list without
// duplication.
const std::vector<std::string> &system_dirs() {
  static const std::vector<std::string> dirs = {
      "/etc",  "/root", "/var", "/proc",  "/sys",       "/dev",
      "/boot", "/lib",  "/usr", "/sbin",  "/bin",       "/run",
      "/opt",  "/tmp",  "/srv", "/media", "/lost+found"};
  return dirs;
}

// Returns the matching SYSTEM_DIR prefix for string `s`, or "" if none.
// `s` must already be canonical/weakly_canonical resolved (absolute).
std::string match_system_dir_string(const std::string &s) {
  for (const auto &d : system_dirs()) {
    if (s == d)
      return d;
    if (s.length() > d.length() && s[d.length()] == '/' &&
        s.substr(0, d.length()) == d) {
      return d;
    }
  }
  return "";
}

// Resolve `p` to a string suitable for SYSTEM_DIRS comparison, honoring the
// same precedence as validate_path_allowed(): canonical if it exists, else
// weakly_canonical of the parent chain. Returns nullopt if resolution fails.
std::optional<std::string> resolve_for_system_dirs(const fs::path &p) {
  auto resolved = safe_canonical(p);
  if (!resolved.empty()) {
    return resolved.string();
  }
  std::error_code ec;
  fs::path weak_resolved = fs::weakly_canonical(p, ec);
  if (ec)
    return std::nullopt;
  return weak_resolved.string();
}

} // namespace

// Resolve path to canonical form using fs::canonical().  Returns empty path
// if the path does not exist or cannot be resolved — callers must treat this
// as a validation failure.  We intentionally do NOT fall back to
// weakly_canonical because it does not resolve symlinks, leaving ".."
// components unresolved in some cases, which could bypass SYSTEM_DIRS checks.
fs::path safe_canonical(const fs::path &p) {
  std::error_code ec;
  auto canonical = fs::canonical(p, ec);
  if (!ec)
    return canonical;
  return fs::path{};
}

bool validate_path_allowed(const fs::path &p) {
  if (p.empty())
    return false;
  return !matched_system_dir(p).has_value();
}

std::optional<std::string> matched_system_dir(const fs::path &p) {
  if (p.empty())
    return std::nullopt;

  auto resolved = resolve_for_system_dirs(p);
  if (!resolved.has_value())
    return std::nullopt;

  std::string d = match_system_dir_string(*resolved);
  if (d.empty())
    return std::nullopt;
  return d;
}

// Same as validate_path_allowed but skips SYSTEM_DIRS blacklist check.
// For allowed_bases paths that are explicitly configured (e.g. BeeGFS,
// /scratch).
bool validate_path_allowed_skip_system_dirs(const fs::path &p) {
  if (p.empty())
    return false;

  // Only reject trivially dangerous paths: ".." components
  for (const auto &part : p) {
    if (part == "..")
      return false;
  }

  return true;
}

bool validate_mount_source(const fs::path &source) {
  if (source.empty())
    return false;

  std::string main_user = utils::get_effective_username();
  if (utils::is_path_allowed(source, main_user)) {
    return true;
  }

  return validate_path_allowed(source);
}

bool is_subpath(const fs::path &parent, const fs::path &child) {
  auto norm_parent = safe_canonical(parent);
  auto norm_child = safe_canonical(child);

  auto parent_str = norm_parent.string();
  auto child_str = norm_child.string();

  if (parent_str == child_str) {
    return true;
  }

  if (child_str.length() > parent_str.length()) {
    return child_str.substr(0, parent_str.length() + 1) == parent_str + "/";
  }

  return false;
}

PathCheckResult validate_mount_paths(const fs::path &source,
                                     const fs::path &target) {
  if (source.empty() || target.empty()) {
    return {false, "Source and target paths must not be empty"};
  }

  auto norm_source = safe_canonical(source);
  auto norm_target = safe_canonical(target);

  if (norm_source == norm_target) {
    return {false,
            "Source and target are the same path: " + norm_source.string()};
  }

  if (is_subpath(source, target)) {
    return {false, "Target (" + target.string() +
                       ") is a subdirectory of source (" + source.string() +
                       ")"};
  }

  if (is_subpath(target, source)) {
    return {false, "Source (" + source.string() +
                       ") is a subdirectory of target (" + target.string() +
                       ")"};
  }

  std::error_code ec;
  if (!fs::exists(source, ec)) {
    return {false, "Source path does not exist: " + source.string()};
  }

  return {true, "OK"};
}

PathCheckResult validate_no_circular_mount(const fs::path &source,
                                           const fs::path &target) {
  std::error_code ec;

  auto norm_source = safe_canonical(source);
  auto norm_target = safe_canonical(target);

  if (is_subpath(norm_source, norm_target)) {
    return {false, "Circular mount detected: source is under target"};
  }

  return {true, "OK"};
}

bool validate_path_exists(const fs::path &p) {
  if (p.empty())
    return false;

  int fd = ::open(p.c_str(), O_PATH | O_NOFOLLOW);
  if (fd < 0)
    return false;

  struct stat st{};
  int ret = ::fstat(fd, &st);
  ::close(fd);

  if (ret < 0)
    return false;

  return S_ISDIR(st.st_mode) || S_ISREG(st.st_mode);
}

bool safe_create_directories(const fs::path &p) {
  if (p.empty())
    return true;

  std::error_code ec;
  if (fs::exists(p, ec))
    return true;

  std::vector<std::string> parts;
  fs::path cur = p;
  while (!cur.empty()) {
    parts.insert(parts.begin(), cur.filename().string());
    cur = cur.parent_path();
    if (cur == "/")
      break;
  }

  bool is_absolute = p.is_absolute();
  int dirfd = AT_FDCWD;
  int root_fd = -1;
  if (is_absolute) {
    root_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
      utils::get_logger()->error(
          "safe_create_directories: cannot open root '/': {}", strerror(errno));
      return false;
    }
    dirfd = root_fd;
  }

  int owned_fd = root_fd;

  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string &part = parts[i];
    int fd = openat(dirfd, part.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      if (errno == ENOENT && i + 1 <= parts.size()) {
        if (mkdirat(dirfd, part.c_str(), 0755) != 0) {
          if (errno != EEXIST) {
            utils::get_logger()->error(
                "safe_create_directories: mkdirat {} failed: {}", part,
                strerror(errno));
            if (owned_fd >= 0)
              close(owned_fd);
            return false;
          }
        }
        fd = openat(dirfd, part.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
          struct stat st;
          if (fstatat(dirfd, part.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
              S_ISLNK(st.st_mode)) {
            utils::get_logger()->error(
                "safe_create_directories: TOCTOU - directory '{}' replaced by "
                "symlink after mkdirat",
                part);
          } else {
            utils::get_logger()->error(
                "safe_create_directories: openat {} after mkdir: {}", part,
                strerror(errno));
          }
          if (owned_fd >= 0)
            close(owned_fd);
          return false;
        }
      } else if (errno == ELOOP) {
        utils::get_logger()->error("safe_create_directories: symlink found at "
                                   "component '{}', rejecting",
                                   part);
        if (owned_fd >= 0)
          close(owned_fd);
        return false;
      } else {
        utils::get_logger()->error(
            "safe_create_directories: openat {} failed: {}", part,
            strerror(errno));
        if (owned_fd >= 0)
          close(owned_fd);
        return false;
      }
    }

    if (owned_fd >= 0)
      close(owned_fd);
    owned_fd = fd;
    dirfd = fd;
  }

  if (owned_fd >= 0)
    close(owned_fd);
  return true;
}

} // namespace ai_mirror::security

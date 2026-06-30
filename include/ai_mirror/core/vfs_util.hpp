#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace ai_mirror::core {

/// Known virtual/pseudo filesystem types from /proc/mounts column 3.
/// These filesystems have no real device backing them — their "source"
/// column is a virtual device name (proc, tmpfs, none, etc.) rather than
/// a device path or network path.  Source is from Linux kernel docs
/// (Documentation/filesystems/) and common practice.
///
/// When a mount has one of these fstypes, we skip ::stat() on its source
/// and omit source_stat from serialized output.
inline const std::unordered_set<std::string> &virtual_fstypes() {
  static const std::unordered_set<std::string> types{
      "proc",    "tmpfs",   "devtmpfs",    "sysfs",        "cgroup",
      "cgroup2", "devpts",  "none",        "binfmt_misc",  "configfs",
      "debugfs", "tracefs", "securityfs",  "pstore",       "hugetlbfs",
      "mqueue",  "fusectl", "efivarfs",    "bpf",          "autofs",
      "overlay", "aufs",    "fuse.portal", "beegfs_nodev",
  };
  return types;
}

/// Returns true if the given fstype is a known virtual filesystem.
/// Virtual filesystems have no real device backing — stat() on their
/// "source" column would fail or return all-zero stat data.
inline bool is_virtual_fstype(const std::string &fstype) {
  return virtual_fstypes().count(fstype) > 0;
}

/// Fallback heuristic: if fstype is unknown/empty, check if the source
/// path looks like a real path (starts with '/') or a network path
/// (contains ':').  Returns false for empty source and virtual device
/// names (proc, tmpfs, etc.).
inline bool is_virtual_source_fallback(const std::string &source) {
  // Empty source → virtual (cleared by update_state_mounts)
  if (source.empty())
    return true;
  // Real paths start with '/', network paths like //server/share start with '/'
  if (source[0] == '/')
    return false;
  // Network filesystems (NFS: server:/path, CIFS: //server/share)
  // // is handled above (starts with '/'), so remaining non-'/' sources
  // are virtual device names like proc, tmpfs, beegfs_nodev, or NFS paths.
  // NFS sources (server:/path) do not start with '/' — we rely on fstype
  // to distinguish NFS (nfs4, nfs) from virtual devices.
  // Without fstype, assume virtual (conservative: skip stat rather than
  // stat a pseudo-device name that would always fail).
  return true;
}

/// Combined check: use fstype if available, fall back to source heuristic.
inline bool is_virtual_source(const std::string &source,
                              const std::string &fstype) {
  if (!fstype.empty()) {
    return is_virtual_fstype(fstype);
  }
  return is_virtual_source_fallback(source);
}

} // namespace ai_mirror::core

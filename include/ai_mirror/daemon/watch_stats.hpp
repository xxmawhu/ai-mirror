#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace ai_mirror::daemon {

// Process info parsed from /proc/[pid]/status
struct ProcInfo {
  uid_t uid;
  unsigned long vm_rss_kb; // Resident Set Size in KB
};

// Aggregated stats per UID (from single /proc scan)
struct UidStats {
  int process_count = 0;
  unsigned long memory_mb = 0;
  double cpu_percent = 0.0;
  bool has_sshd = false;
};

// User statistics for TUI display
struct UserStats {
  uid_t uid;
  std::string username;
  int process_count = 0;
  unsigned long memory_mb = 0;
  double cpu_percent = 0.0;
  bool logged_in = false;
};

// Parse /proc/[pid]/status to get UID and memory
// Returns nullopt if file cannot be read or parsed
std::optional<ProcInfo>
read_proc_status(const std::filesystem::path &status_path);

// Gather all UID stats with single /proc scan + single ps call
// Returns map: uid -> {procs, mem, cpu, sshd}
std::unordered_map<uid_t, UidStats> gather_all_uid_stats();

// Convert UID stats map to UserStats vector for given users
// Sorts by CPU usage (highest first)
std::vector<UserStats>
build_user_stats(const std::vector<std::string> &usernames,
                 const std::vector<uid_t> &uids,
                 const std::unordered_map<uid_t, UidStats> &uid_stats);

// Legacy function for backwards compatibility (uses gather_all_uid_stats
// internally)
std::vector<UserStats>
gather_user_stats(const std::vector<std::string> &usernames,
                  const std::vector<uid_t> &uids);

} // namespace ai_mirror::daemon

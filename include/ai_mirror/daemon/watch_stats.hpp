#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <sys/types.h>

namespace ai_mirror::daemon {

// Process info parsed from /proc/[pid]/status
struct ProcInfo {
    uid_t uid;
    unsigned long vm_rss_kb;  // Resident Set Size in KB
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
std::optional<ProcInfo> read_proc_status(const std::filesystem::path& status_path);

// Check if user has SSH session active
// Uses ps -u <username> to find sshd processes
bool check_ssh_session(const std::string& username);

// Gather statistics for all users in the list
// Iterates /proc to count processes, memory, and CPU usage per user
// Sorts by CPU usage (highest first)
std::vector<UserStats> gather_user_stats(
    const std::vector<std::string>& usernames,
    const std::vector<uid_t>& uids
);

} // namespace ai_mirror::daemon
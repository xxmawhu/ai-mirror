#include "ai_mirror/daemon/watch_stats.hpp"
#include "ai_mirror/utils/shell.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace ai_mirror::daemon {

namespace fs = std::filesystem;

std::optional<ProcInfo> read_proc_status(const fs::path& status_path) {
    std::ifstream f(status_path);
    if (!f.is_open()) return std::nullopt;

    ProcInfo info{};
    info.uid = static_cast<uid_t>(-1);
    info.vm_rss_kb = 0;
    bool found_uid = false;

    std::string line;
    while (std::getline(f, line)) {
        if (line.find("Uid:") == 0) {
            std::istringstream iss(line.substr(5));
            iss >> info.uid;
            found_uid = true;
        } else if (line.find("VmRSS:") == 0) {
            std::istringstream iss(line.substr(7));
            iss >> info.vm_rss_kb;
        }
    }

    if (!found_uid) return std::nullopt;
    return info;
}

// Single-pass /proc scan: build uid→{procs, mem} map
// Cost: 1 directory iteration + N file reads (N = total PIDs)
// Instead of M users × N PIDs
static std::unordered_map<uid_t, std::pair<int, unsigned long>> scan_proc_by_uid() {
    std::unordered_map<uid_t, std::pair<int, unsigned long>> result;
    fs::path proc_path("/proc");

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(proc_path, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;

        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] < '0' || name[0] > '9') continue;

        auto proc_info = read_proc_status(entry.path() / "status");
        if (!proc_info) continue;

        auto& [count, mem_kb] = result[proc_info->uid];
        count++;
        mem_kb += proc_info->vm_rss_kb;
    }

    return result;
}

// Single ps call: get CPU % and SSH detection for all processes at once
// Cost: 1 fork+exec instead of 2 per user
struct PsAllData {
    std::unordered_map<uid_t, double> cpu_by_uid;
    std::unordered_set<uid_t> sshd_uids;
};

static PsAllData scan_ps_all() {
    PsAllData data;

    // Single ps call: %cpu + uid
    auto cpu_result = utils::exec_safe({"ps", "-eo", "%cpu,uid", "--no-headers"});
    if (cpu_result.exit_code == 0) {
        std::istringstream iss(cpu_result.stdout_output);
        std::string line;
        while (std::getline(iss, line)) {
            try {
                size_t pos = line.find_last_of(" \t");
                if (pos == std::string::npos) continue;
                double cpu = std::stod(line.substr(0, pos));
                uid_t uid = static_cast<uid_t>(std::stoul(line.substr(pos + 1)));
                data.cpu_by_uid[uid] += cpu;
            } catch (...) {}
        }
    }

    // Single ps call: comm + uid (for SSH detection)
    auto comm_result = utils::exec_safe({"ps", "-eo", "comm,uid", "--no-headers"});
    if (comm_result.exit_code == 0) {
        std::istringstream iss(comm_result.stdout_output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("sshd") != std::string::npos) {
                size_t pos = line.find_last_of(" \t");
                if (pos != std::string::npos) {
                    try {
                        uid_t uid = static_cast<uid_t>(std::stoul(line.substr(pos + 1)));
                        data.sshd_uids.insert(uid);
                    } catch (...) {}
                }
            }
        }
    }

    return data;
}

std::unordered_map<uid_t, UidStats> gather_all_uid_stats() {
    // 1. Single /proc scan for process count + memory
    auto proc_map = scan_proc_by_uid();

    // 2. Single ps call pair for CPU + SSH
    auto ps_data = scan_ps_all();

    // 3. Merge into final map
    std::unordered_map<uid_t, UidStats> result;

    // Collect all UIDs from both sources
    for (const auto& [uid, _] : proc_map) {
        result[uid]; // ensure entry exists
    }
    for (const auto& [uid, _] : ps_data.cpu_by_uid) {
        result[uid];
    }

    for (auto& [uid, stats] : result) {
        if (auto it = proc_map.find(uid); it != proc_map.end()) {
            stats.process_count = it->second.first;
            stats.memory_mb = it->second.second / 1024;
        }
        if (auto it = ps_data.cpu_by_uid.find(uid); it != ps_data.cpu_by_uid.end()) {
            stats.cpu_percent = it->second;
        }
        stats.has_sshd = ps_data.sshd_uids.count(uid) > 0;
    }

    return result;
}

std::vector<UserStats> build_user_stats(
    const std::vector<std::string>& usernames,
    const std::vector<uid_t>& uids,
    const std::unordered_map<uid_t, UidStats>& uid_stats
) {
    if (usernames.size() != uids.size()) return {};

    std::vector<UserStats> stats;
    stats.reserve(usernames.size());

    for (size_t i = 0; i < usernames.size(); ++i) {
        UserStats s;
        s.uid = uids[i];
        s.username = usernames[i];

        auto it = uid_stats.find(uids[i]);
        if (it != uid_stats.end()) {
            s.process_count = it->second.process_count;
            s.memory_mb = it->second.memory_mb;
            s.cpu_percent = it->second.cpu_percent;
            s.logged_in = it->second.has_sshd;
        }

        stats.push_back(s);
    }

    std::sort(stats.begin(), stats.end(),
        [](const UserStats& a, const UserStats& b) {
            return a.cpu_percent > b.cpu_percent;
        });

    return stats;
}

// Legacy wrapper
std::vector<UserStats> gather_user_stats(
    const std::vector<std::string>& usernames,
    const std::vector<uid_t>& uids
) {
    auto uid_stats = gather_all_uid_stats();
    return build_user_stats(usernames, uids, uid_stats);
}

} // namespace ai_mirror::daemon
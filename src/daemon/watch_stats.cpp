#include "ai_mirror/daemon/watch_stats.hpp"
#include "ai_mirror/utils/shell.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

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

bool check_ssh_session(const std::string& username) {
    auto result = utils::exec_safe({"ps", "-u", username, "-o", "comm="});
    if (result.exit_code != 0) return false;
    std::istringstream iss(result.stdout_output);
    std::string comm;
    while (std::getline(iss, comm)) {
        if (comm.find("sshd") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<UserStats> gather_user_stats(
    const std::vector<std::string>& usernames,
    const std::vector<uid_t>& uids
) {
    if (usernames.size() != uids.size()) return {};

    std::vector<UserStats> stats;
    fs::path proc_path("/proc");

    for (size_t i = 0; i < usernames.size(); ++i) {
        UserStats s;
        s.uid = uids[i];
        s.username = usernames[i];
        s.process_count = 0;
        s.memory_mb = 0;
        s.cpu_percent = 0.0;
        s.logged_in = false;

        // Scan /proc for processes belonging to this user
        if (fs::exists(proc_path)) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(proc_path, ec)) {
                if (ec) break;
                if (!entry.is_directory()) continue;
                std::string name = entry.path().filename().string();
                if (name.empty() || name[0] < '0' || name[0] > '9') continue;

                auto proc_info = read_proc_status(entry.path() / "status");
                if (!proc_info) continue;

                if (proc_info->uid == s.uid) {
                    s.process_count++;
                    s.memory_mb += proc_info->vm_rss_kb / 1024;
                }
            }
        }

        // Get CPU usage via ps
        auto result = utils::exec_safe({"ps", "-u", usernames[i], "-o", "%cpu="});
        if (result.exit_code == 0 && !result.stdout_output.empty()) {
            std::istringstream iss(result.stdout_output);
            std::string cpu_str;
            double total_cpu = 0.0;
            while (std::getline(iss, cpu_str)) {
                try {
                    total_cpu += std::stod(cpu_str);
                } catch (...) {}
            }
            s.cpu_percent = total_cpu;
        }

        // Check SSH session
        s.logged_in = check_ssh_session(usernames[i]);

        stats.push_back(s);
    }

    // Sort by CPU usage (highest first)
    std::sort(stats.begin(), stats.end(),
        [](const UserStats& a, const UserStats& b) {
            return a.cpu_percent > b.cpu_percent;
        });

    return stats;
}

} // namespace ai_mirror::daemon
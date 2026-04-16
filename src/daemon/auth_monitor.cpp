#include "ai_mirror/daemon/auth_monitor.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>

namespace ai_mirror::daemon {

static const std::set<std::string> VALID_MONTHS = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

AuthMonitor::AuthMonitor(const std::string& auth_log_path)
    : auth_log_path_(auth_log_path) {}

void AuthMonitor::start(Callback on_event) {
    running_ = true;
    std::ifstream log_file(auth_log_path_);
    if (!log_file.is_open()) {
        utils::get_logger()->error("Cannot open auth log: {}", auth_log_path_);
        return;
    }

    log_file.seekg(0, std::ios::end);

    while (running_) {
        std::string line;
        while (std::getline(log_file, line)) {
            if (line.empty()) continue;

            AuthEvent event;
            std::istringstream iss(line);

            std::string month, day, time;
            iss >> month >> day >> time;

            if (VALID_MONTHS.find(month) == VALID_MONTHS.end()) {
                continue;
            }

            event.timestamp = month + " " + day + " " + time;

            std::string rest;
            std::getline(iss, rest);

            for (const auto& keyword : {"Accepted", "Failed", "session opened", "session closed"}) {
                auto pos = rest.find(keyword);
                if (pos != std::string::npos) {
                    event.action = keyword;

                    auto user_pos = rest.find("for ", pos);
                    // Validate username before storing: auth log entries could be spoofed or
                    // contain malformed data from a compromised log source.  Reject
                    // usernames with shell metacharacters to prevent injection if
                    // the username is later used in shell commands.
                    if (user_pos != std::string::npos) {
                        std::istringstream user_stream(rest.substr(user_pos + 4));
                        std::string user;
                        user_stream >> user;
                        if (!utils::validate_username(user)) {
                            utils::get_logger()->warn("AuthMonitor: skipping event with invalid username '{}' in auth log", user);
                            break;
                        }
                        event.username = user;
                    }
                    break;
                }
            }

            if (!event.action.empty()) {
                on_event(event);
            }
        }

        if (!running_) break;

        log_file.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void AuthMonitor::stop() {
    running_ = false;
}

// Maximum number of events to return from get_recent_events().  Prevents
// memory exhaustion if caller requests an unreasonably large count or if
// the auth log file is extremely large.
static constexpr int MAX_EVENTS_COUNT = 1000;

std::vector<AuthEvent> AuthMonitor::get_recent_events(int count) {
    count = std::min(count, MAX_EVENTS_COUNT);
    if (count <= 0) count = 10;
    
    std::vector<AuthEvent> events;
    std::ifstream log_file(auth_log_path_);
    if (!log_file.is_open()) return events;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(log_file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    int start = std::max(0, static_cast<int>(lines.size()) - count);
    for (int i = start; i < static_cast<int>(lines.size()); ++i) {
        AuthEvent event;
        std::istringstream iss(lines[i]);

        std::string month, day, time;
        iss >> month >> day >> time;
        event.timestamp = month + " " + day + " " + time;

        std::string rest;
        std::getline(iss, rest);
        event.detail = rest;

        events.push_back(event);
    }

    return events;
}

}

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

static std::vector<std::string> read_last_n_lines(const std::string& path, int n) {
    std::vector<std::string> result;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return result;

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();
    if (file_size <= 0) return result;

    std::string line_buf;
    std::string chunk;
    const int buf_size = 4096;
    char buf[buf_size];

    auto pos = file_size;
    while (pos > 0 && static_cast<int>(result.size()) < n) {
        auto read_size = std::min(static_cast<int>(pos), buf_size);
        pos -= read_size;
        file.seekg(pos);
        file.read(buf, read_size);
        chunk.assign(buf, read_size);

        for (int i = static_cast<int>(chunk.size()) - 1; i >= 0; --i) {
            if (chunk[i] == '\n') {
                if (!line_buf.empty()) {
                    std::reverse(line_buf.begin(), line_buf.end());
                    result.push_back(line_buf);
                    line_buf.clear();
                    if (static_cast<int>(result.size()) >= n) break;
                }
            } else {
                line_buf.push_back(chunk[i]);
            }
        }
    }
    if (!line_buf.empty() && static_cast<int>(result.size()) < n) {
        std::reverse(line_buf.begin(), line_buf.end());
        result.push_back(line_buf);
    }

    std::reverse(result.begin(), result.end());
    return result;
}

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
    auto lines = read_last_n_lines(auth_log_path_, count * 3);

    for (const auto& line : lines) {
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
            event.detail = rest;
            events.push_back(event);
            if (static_cast<int>(events.size()) >= count) break;
        }
    }

    return events;
}

}

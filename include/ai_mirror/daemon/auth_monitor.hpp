#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ai_mirror::daemon {

struct AuthEvent {
    std::string timestamp;
    std::string username;
    std::string action;
    std::string detail;
};

class AuthMonitor {
public:
    using Callback = std::function<void(const AuthEvent&)>;

    explicit AuthMonitor(const std::string& auth_log_path);
    void start(Callback on_event);
    void stop();
    std::vector<AuthEvent> get_recent_events(int count = 100);

private:
    std::string auth_log_path_;
    bool running_ = false;
};

}

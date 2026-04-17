#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace ai_mirror::daemon {

struct HealthStatus {
    std::string mount_point;
    bool healthy;
    std::string detail;
};

class HealthCheck {
public:
    std::vector<HealthStatus> check_all();
    HealthStatus check_mount(const std::string& mount_point);
    int run_periodic(int interval_seconds);
    void stop();

private:
    std::atomic<bool> running_{true};
    std::condition_variable cv_;
    std::mutex mtx_;
    static std::atomic<HealthCheck*> instance_;
    static std::atomic<bool> signal_received_;
    static void signal_handler(int sig);
};

}

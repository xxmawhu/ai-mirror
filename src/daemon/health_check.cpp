#include "ai_mirror/daemon/health_check.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <atomic>
#include <csignal>
#include <condition_variable>
#include <mutex>

namespace ai_mirror::daemon {

// Graceful shutdown: signal handler sets running_ = false and wakes the
// condition variable so run_periodic() exits its loop promptly instead of
// waiting for the full sleep interval.  This ensures SIGTERM/SIGINT are
// handled within one health check cycle.
static std::atomic<bool> running_{true};
static std::condition_variable cv_;
static std::mutex mtx_;

static void signal_handler([[maybe_unused]] int sig) {
    running_ = false;
    cv_.notify_all();
}

std::vector<HealthStatus> HealthCheck::check_all() {
    core::Graft graft;
    auto issues = graft.health_check();

    std::vector<HealthStatus> statuses;
    for (const auto& m : issues) {
        HealthStatus s;
        s.mount_point = m.target.string();
        s.detail = m.active ? "OK" : "Source missing: " + m.source.string();
        s.healthy = m.active;
        statuses.push_back(s);

        if (!m.active) {
            utils::get_logger()->warn("Unhealthy mount: {} -> {}", m.source.string(), m.target.string());
        }
    }

    return statuses;
}

HealthStatus HealthCheck::check_mount(const std::string& mount_point) {
    core::Graft graft;
    HealthStatus s;
    s.mount_point = mount_point;
    s.healthy = graft.is_mounted(std::filesystem::path(mount_point));
    s.detail = s.healthy ? "Mounted" : "Not mounted";
    return s;
}

int HealthCheck::run_periodic(int interval_seconds) {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    utils::get_logger()->info("Health check running every {} seconds", interval_seconds);

    while (running_) {
        auto statuses = check_all();
        int unhealthy = 0;
        for (const auto& s : statuses) {
            if (!s.healthy) unhealthy++;
        }

        if (unhealthy > 0) {
            utils::get_logger()->warn("Health check: {}/{} mounts unhealthy", unhealthy, statuses.size());
        } else {
            utils::get_logger()->debug("Health check: all {} mounts healthy", statuses.size());
        }

        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::seconds(interval_seconds), [] { return !running_.load(); });
    }

    utils::get_logger()->info("Health check shutting down gracefully");
    return 0;
}

}

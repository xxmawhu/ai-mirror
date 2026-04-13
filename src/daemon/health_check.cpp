#include "ai_mirror/daemon/health_check.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>

namespace ai_mirror::daemon {

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
    utils::get_logger()->info("Health check running every {} seconds", interval_seconds);

    while (true) {
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

        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
    }

    return 0;
}

}

#pragma once

#include <string>
#include <vector>

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
};

}

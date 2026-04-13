#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ai_mirror::daemon {

std::vector<fs::path> MountCleaner::find_stale_mounts() {
    std::vector<fs::path> stale;
    std::ifstream mounts("/proc/mounts");
    std::string line;

    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point;
        iss >> device >> mount_point;

        if (mount_point.find("/home/i") != std::string::npos) {
            std::error_code ec;
            if (!fs::exists(device, ec)) {
                stale.push_back(fs::path(mount_point));
                utils::get_logger()->warn("Stale mount found: {}", mount_point);
            }
        }
    }

    return stale;
}

int MountCleaner::force_cleanup(const std::vector<fs::path>& mounts) {
    int cleaned = 0;
    for (const auto& m : mounts) {
        std::ostringstream cmd;
        cmd << "umount -l " << m.string();
        auto result = utils::execute(cmd.str());
        if (result.exit_code == 0) {
            cleaned++;
            utils::get_logger()->info("Lazy unmounted: {}", m.string());
        } else {
            utils::get_logger()->error("Failed to unmount {}: {}", m.string(), result.stderr_output);
        }
    }
    return cleaned;
}

int MountCleaner::cleanup_for_user(const std::string& username) {
    std::vector<fs::path> user_mounts;

    std::ifstream mounts("/proc/mounts");
    std::string line;
    std::string user_home = "/home/" + username;

    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point;
        iss >> device >> mount_point;

        if (mount_point.find(user_home) == 0) {
            user_mounts.push_back(fs::path(mount_point));
        }
    }

    return force_cleanup(user_mounts);
}

}

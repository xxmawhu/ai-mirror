#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>

namespace ai_mirror::core {

Graft::Graft() = default;

bool Graft::execute_mount(const fs::path& source, const fs::path& target, bool read_only) {
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        fs::create_directories(target, ec);
        if (ec) {
            utils::get_logger()->error("Failed to create mount target: {}", target.string());
            return false;
        }
    }

    auto validation = security::validate_mount_paths(source, target);
    if (!validation.safe) {
        utils::get_logger()->error("Mount validation failed: {}", validation.reason);
        return false;
    }

    std::ostringstream cmd;
    cmd << "mount --bind " << source.string() << " " << target.string();
    auto result = utils::execute(cmd.str());
    if (result.exit_code != 0) {
        utils::get_logger()->error("mount --bind failed: {}", result.stderr_output);
        return false;
    }

    if (read_only) {
        std::ostringstream remount_cmd;
        remount_cmd << "mount -o remount,bind,ro " << target.string();
        auto remount_result = utils::execute(remount_cmd.str());
        if (remount_result.exit_code != 0) {
            utils::get_logger()->warn("Failed to remount as read-only: {}", remount_result.stderr_output);
        }
    }

    utils::get_logger()->info("Bind mounted {} -> {} (ro={})", source.string(), target.string(), read_only);
    return true;
}

bool Graft::execute_umount(const fs::path& target, bool lazy) {
    std::ostringstream cmd;
    cmd << "umount";
    if (lazy) {
        cmd << " -l";
    }
    cmd << " " << target.string();

    auto result = utils::execute(cmd.str());
    if (result.exit_code != 0) {
        utils::get_logger()->error("umount failed for {}: {}", target.string(), result.stderr_output);
        return false;
    }

    utils::get_logger()->info("Unmounted {} (lazy={})", target.string(), lazy);
    return true;
}

std::vector<MountEntry> Graft::parse_mount_table() const {
    std::vector<MountEntry> entries;
    std::ifstream mounts("/proc/mounts");
    std::string line;

    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point, fs_type, options;
        iss >> device >> mount_point >> fs_type >> options;

        if (mount_point.find("/home/i") != std::string::npos) {
            MountEntry entry;
            entry.source = device;
            entry.target = mount_point;
            entry.read_only = options.find("ro") != std::string::npos;
            entry.active = true;
            entries.push_back(entry);
        }
    }

    return entries;
}

bool Graft::bind_mount(const fs::path& source, const fs::path& target, bool read_only) {
    if (is_mounted(target)) {
        utils::get_logger()->warn("Target already mounted: {}", target.string());
        return true;
    }
    return execute_mount(source, target, read_only);
}

bool Graft::unmount(const fs::path& target, bool lazy) {
    return execute_umount(target, lazy);
}

bool Graft::unmount_all(const std::string& username) {
    auto mounts = parse_mount_table();
    bool all_ok = true;

    std::string user_home = "/home/" + username;
    for (const auto& m : mounts) {
        if (m.target.string().find(user_home) == 0) {
            if (!unmount(m.target, false)) {
                all_ok = false;
            }
        }
    }

    return all_ok;
}

std::vector<MountEntry> Graft::list_mounts(const std::string& username) const {
    auto all = parse_mount_table();
    std::string user_home = "/home/" + username;

    std::vector<MountEntry> user_mounts;
    std::copy_if(all.begin(), all.end(), std::back_inserter(user_mounts),
                 [&](const MountEntry& m) {
                     return m.target.string().find(user_home) == 0;
                 });

    return user_mounts;
}

bool Graft::is_mounted(const fs::path& target) const {
    auto mounts = parse_mount_table();
    auto canon_target = security::safe_canonical(target);
    for (const auto& m : mounts) {
        if (security::safe_canonical(m.target) == canon_target) {
            return true;
        }
    }
    return false;
}

bool Graft::ensure_group_exists(const std::string& groupname) {
    auto result = utils::execute("getent group " + groupname);
    if (result.exit_code != 0) {
        result = utils::execute("groupadd " + groupname);
        if (result.exit_code != 0) {
            utils::get_logger()->error("groupadd failed for {}: {}", groupname, result.stderr_output);
            return false;
        }
        utils::get_logger()->info("Created group: {}", groupname);
    }
    return true;
}

bool Graft::set_directory_group(const fs::path& path, const std::string& groupname) {
    auto result = utils::execute("chgrp " + groupname + " " + path.string());
    if (result.exit_code != 0) {
        utils::get_logger()->error("chgrp failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

bool Graft::set_sgid(const fs::path& path) {
    auto result = utils::execute("chmod g+s " + path.string());
    if (result.exit_code != 0) {
        utils::get_logger()->error("chmod g+s failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

bool Graft::grant_write_access(const fs::path& path, const std::string& username) {
    if (!ensure_group_exists(username)) {
        return false;
    }

    auto result = utils::execute("usermod -aG " + username + " " + username);
    if (result.exit_code != 0) {
        utils::get_logger()->error("usermod -aG failed: {}", result.stderr_output);
        return false;
    }

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        fs::create_directories(path, ec);
        if (ec) {
            utils::get_logger()->error("Failed to create directory: {}", path.string());
            return false;
        }
    }

    if (!set_directory_group(path, username)) {
        return false;
    }

    auto chmod_result = utils::execute("chmod g+rwX " + path.string());
    if (chmod_result.exit_code != 0) {
        utils::get_logger()->error("chmod g+rwX failed: {}", chmod_result.stderr_output);
        return false;
    }

    if (fs::is_directory(path)) {
        set_sgid(path);
    }

    utils::get_logger()->info("Granted group write access: {} -> group {}", path.string(), username);
    return true;
}

bool Graft::revoke_write_access(const fs::path& path, [[maybe_unused]] const std::string& username) {
    auto result = utils::execute("chmod g-rwx " + path.string());
    if (result.exit_code != 0) {
        utils::get_logger()->error("chmod g-rwx failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

std::vector<MountEntry> Graft::health_check() const {
    auto mounts = parse_mount_table();
    std::vector<MountEntry> issues;

    for (const auto& m : mounts) {
        std::error_code ec;
        if (!fs::exists(m.source, ec)) {
            MountEntry broken = m;
            broken.active = false;
            issues.push_back(broken);
        }
    }

    return issues;
}

int Graft::force_cleanup(const std::vector<fs::path>& dead_mounts) {
    int cleaned = 0;
    for (const auto& m : dead_mounts) {
        if (unmount(m, true)) {
            cleaned++;
        }
    }
    return cleaned;
}

}

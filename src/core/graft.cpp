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

Graft::Graft(const std::string& user_prefix) : prefix_(user_prefix) {}

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

    auto result = utils::exec_safe({"mount", "--bind", source.string(), target.string()});
    if (result.exit_code != 0) {
        utils::get_logger()->error("mount --bind failed: {}", result.stderr_output);
        return false;
    }

    if (read_only) {
        auto remount_result = utils::exec_safe({"mount", "-o", "remount,bind,ro", target.string()});
        if (remount_result.exit_code != 0) {
            utils::get_logger()->warn("Failed to remount as read-only: {}", remount_result.stderr_output);
        }
    }

    utils::get_logger()->info("Bind mounted {} -> {} (ro={})", source.string(), target.string(), read_only);
    return true;
}

bool Graft::execute_umount(const fs::path& target, bool lazy) {
    std::vector<std::string> args;
    args.reserve(3);
    args.push_back("umount");
    if (lazy) {
        args.push_back("-l");
    }
    args.push_back(target.string());

    auto result = utils::exec_safe(args);
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

        if (mount_point.find("/home/" + prefix_) == 0) {
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
    if (!security::validate_mount_source(source)) {
        utils::get_logger()->error("Mount source rejected (system path): {}", source.string());
        return false;
    }

    auto pre_mount_source = security::safe_canonical(source);
    if (pre_mount_source.empty()) {
        utils::get_logger()->error("Mount source canonical resolution failed: {}", source.string());
        return false;
    }

    if (is_mounted(target)) {
        utils::get_logger()->warn("Target already mounted: {}", target.string());
        return true;
    }

    if (!security::validate_path_exists(source)) {
        utils::get_logger()->error("Mount source path does not exist or is not regular/dir: {}", source.string());
        return false;
    }

    auto pre_exec_source = security::safe_canonical(source);
    if (pre_exec_source != pre_mount_source) {
        utils::get_logger()->error("Mount source path changed between validation and execution: {}", source.string());
        return false;
    }

    return execute_mount(pre_exec_source, target, read_only);
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
    if (!utils::validate_username(groupname)) {
        utils::get_logger()->error("Invalid group name: {}", groupname);
        return false;
    }
    auto result = utils::exec_safe({"getent", "group", groupname});
    if (result.exit_code != 0) {
        result = utils::exec_safe({"groupadd", groupname});
        if (result.exit_code != 0) {
            utils::get_logger()->error("groupadd failed for {}: {}", groupname, result.stderr_output);
            return false;
        }
        utils::get_logger()->info("Created group: {}", groupname);
    }
    return true;
}

bool Graft::set_directory_group(const fs::path& path, const std::string& groupname) {
    auto result = utils::exec_safe({"chgrp", groupname, path.string()});
    if (result.exit_code != 0) {
        utils::get_logger()->error("chgrp failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

bool Graft::set_sgid(const fs::path& path) {
    auto result = utils::exec_safe({"chmod", "g+s", path.string()});
    if (result.exit_code != 0) {
        utils::get_logger()->error("chmod g+s failed: {}", result.stderr_output);
        return false;
    }
    return true;
}

bool Graft::grant_write_access(const fs::path& path, const std::string& username) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for write access: {}", username);
        return false;
    }

    if (!security::validate_path_allowed(path)) {
        utils::get_logger()->error("Grant write path rejected (system directory): {}", path.string());
        return false;
    }

    if (!ensure_group_exists(username)) {
        return false;
    }

    auto mod_result = utils::exec_safe({"usermod", "-aG", username, username});
    if (mod_result.exit_code != 0) {
        utils::get_logger()->error("usermod -aG failed: {}", mod_result.stderr_output);
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

    auto chmod_result = utils::exec_safe({"chmod", "g+rwX", path.string()});
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

bool Graft::revoke_write_access(const fs::path& path, const std::string& username) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for revoke: {}", username);
        return false;
    }

    auto chmod_result = utils::exec_safe({"chmod", "g-s", path.string()});
    if (chmod_result.exit_code != 0) {
        utils::get_logger()->warn("chmod g-s failed (may not be set): {}", chmod_result.stderr_output);
    }

    auto gpasswd_result = utils::exec_safe({"gpasswd", "-d", username, username});
    if (gpasswd_result.exit_code != 0) {
        utils::get_logger()->warn("gpasswd -d failed (user may not be in group): {}", gpasswd_result.stderr_output);
    }

    auto chmod_rw_result = utils::exec_safe({"chmod", "g-rwx", path.string()});
    if (chmod_rw_result.exit_code != 0) {
        utils::get_logger()->error("chmod g-rwx failed: {}", chmod_rw_result.stderr_output);
        return false;
    }

    utils::exec_safe({"groupdel", username});

    utils::get_logger()->info("Revoked write access: {} from group {}", path.string(), username);
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

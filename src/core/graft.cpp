#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/unique_fd.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <sys/mount.h>
#include <grp.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace ai_mirror::core {

using security::safe_create_directories;

Graft::Graft(const std::string& user_prefix) : prefix_(user_prefix) {}

bool Graft::execute_mount(const fs::path& source, const fs::path& target, bool read_only) {
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        if (fs::is_regular_file(source)) {
            fs::path parent = target.parent_path();
            if (!parent.empty() && !fs::exists(parent, ec)) {
                if (!safe_create_directories(parent)) {
                    utils::get_logger()->error("execute_mount: failed to create parent dir for {}", target.string());
                    return false;
                }
            }
            utils::unique_fd ufd(open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
            if (!ufd) {
                utils::get_logger()->error("execute_mount: create target file failed: {} ({})", target.string(), strerror(errno));
                return false;
            }
            ufd.reset();
        } else {
            if (!safe_create_directories(target)) {
                utils::get_logger()->error("execute_mount: failed to create target dir {}", target.string());
                return false;
            }
        }
    }

    auto validation = security::validate_mount_paths(source, target);
    if (!validation.safe) {
        utils::get_logger()->error("Mount validation failed: {}", validation.reason);
        return false;
    }

    if (read_only) {
        auto result = utils::exec_safe({"mount", "--bind", "-o", "ro", source.string(), target.string()});
        if (result.exit_code == 0) {
            utils::get_logger()->info("Bind mounted {} -> {} (ro, single-step)", source.string(), target.string());
            return true;
        }
        utils::get_logger()->warn("Single-step ro mount failed (kernel <5.12?), falling back to two-step: {}", result.stderr_output);
    }

    auto result = utils::exec_safe({"mount", "--bind", source.string(), target.string()});
    if (result.exit_code != 0) {
        if (result.stderr_output.find("busy") != std::string::npos
            || result.stderr_output.find("EBUSY") != std::string::npos) {
            utils::get_logger()->info("Target already mounted (EBUSY): {}", target.string());
            return true;
        }
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

    std::string main_user = utils::get_effective_username();
    std::string main_home = utils::get_effective_home();
    if (main_home.empty()) {
        main_home = utils::get_home_dir(main_user);
    }

    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point, fs_type, options;
        iss >> device >> mount_point >> fs_type >> options;

        std::string expected_prefix = prefix_ + main_user + "_";
        bool is_ai_user_mount = false;
        if (!main_home.empty() && mount_point.find(main_home) == 0) {
            for (size_t i = main_home.length(); i < mount_point.length(); ++i) {
                if (mount_point[i] == '/') {
                    std::string segment = mount_point.substr(0, i);
                    std::string name = fs::path(segment).filename().string();
                    if (name.length() > expected_prefix.length()
                        && name.substr(0, expected_prefix.length()) == expected_prefix
                        && utils::validate_username(name)) {
                        is_ai_user_mount = true;
                        break;
                    }
                }
            }
        }

        if (is_ai_user_mount) {
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
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for unmount_all: {}", username);
        return false;
    }

    std::string prefix_check = prefix_ + utils::get_effective_username() + "_";
    if (username.length() <= prefix_check.length()
        || username.substr(0, prefix_check.length()) != prefix_check) {
        utils::get_logger()->error("Refusing to unmount_all non-ai-mirror user: {}", username);
        return false;
    }

    auto mounts = parse_mount_table();
    bool all_ok = true;

    std::string user_home = utils::get_home_dir(username);
    for (const auto& m : mounts) {
        if (!user_home.empty() && m.target.string().find(user_home) == 0) {
            if (!unmount(m.target, false)) {
                all_ok = false;
            }
        }
    }

    return all_ok;
}

std::vector<MountEntry> Graft::list_mounts(const std::string& username) const {
    auto all = parse_mount_table();
    std::string user_home = utils::get_home_dir(username);

    std::vector<MountEntry> user_mounts;
    std::copy_if(all.begin(), all.end(), std::back_inserter(user_mounts),
                 [&](const MountEntry& m) {
                     return !user_home.empty() && m.target.string().find(user_home) == 0;
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
    struct group* gr = getgrnam(groupname.c_str());
    if (!gr) {
        utils::get_logger()->error("set_directory_group: group '{}' not found", groupname);
        return false;
    }

    utils::unique_fd ufd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (!ufd) {
        if (errno == ELOOP) {
            utils::get_logger()->error("set_directory_group: path is a symlink, rejecting: {}", path.string());
            return false;
        }
        if (errno == ENOTDIR) {
            ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
            if (!ufd) {
                if (errno == ELOOP) {
                    if (lchown(path.c_str(), -1, gr->gr_gid) != 0) {
                        utils::get_logger()->error("set_directory_group: lchown failed for {}: {}", path.string(), strerror(errno));
                        return false;
                    }
                    return true;
                }
                utils::get_logger()->error("set_directory_group: open file failed for {}: {}", path.string(), strerror(errno));
                return false;
            }
        } else {
            utils::get_logger()->error("set_directory_group: open {} failed: {}", path.string(), strerror(errno));
            return false;
        }
    }

    int ret = fchown(ufd.get(), -1, gr->gr_gid);
    ufd.reset();
    if (ret != 0) {
        utils::get_logger()->error("set_directory_group: fchown failed for {}: {}", path.string(), strerror(errno));
        return false;
    }
    return true;
}

bool Graft::set_sgid(const fs::path& path) {
    utils::unique_fd ufd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (!ufd) {
        if (errno == ELOOP) {
            utils::get_logger()->error("set_sgid: path is a symlink, rejecting: {}", path.string());
            return false;
        }
        if (errno == ENOTDIR) {
            ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
            if (!ufd) {
                if (errno == ELOOP) {
                    struct stat st;
                    if (lstat(path.c_str(), &st) == 0) {
                        mode_t new_mode = st.st_mode | S_ISGID;
                        if (fchmodat(AT_FDCWD, path.c_str(), new_mode, AT_SYMLINK_NOFOLLOW) != 0) {
                            utils::get_logger()->error("set_sgid: fchmodat symlink failed for {}: {}", path.string(), strerror(errno));
                            return false;
                        }
                        return true;
                    }
                }
                utils::get_logger()->error("set_sgid: open file failed for {}: {}", path.string(), strerror(errno));
                return false;
            }
        } else {
            utils::get_logger()->error("set_sgid: open {} failed: {}", path.string(), strerror(errno));
            return false;
        }
    }

    struct stat st;
    if (fstat(ufd.get(), &st) != 0) {
        ufd.reset();
        utils::get_logger()->error("set_sgid: fstat failed for {}: {}", path.string(), strerror(errno));
        return false;
    }

    mode_t new_mode = st.st_mode | S_ISGID;
    int ret = fchmod(ufd.get(), new_mode);
    ufd.reset();
    if (ret != 0) {
        utils::get_logger()->error("set_sgid: fchmod failed for {}: {}", path.string(), strerror(errno));
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

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (!safe_create_directories(path)) {
            utils::get_logger()->error("Failed to create directory (safe): {}", path.string());
            return false;
        }
    }

    if (!set_directory_group(path, username)) {
        return false;
    }

    utils::unique_fd ufd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (!ufd) {
        if (errno == ELOOP) {
            utils::get_logger()->error("grant_write_access: path is a symlink, rejecting: {}", path.string());
            return false;
        }
        if (errno == ENOTDIR) {
            ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
            if (!ufd) {
                if (errno == ELOOP) {
                    utils::get_logger()->error("grant_write_access: file path is symlink, rejecting: {}", path.string());
                    return false;
                }
                utils::get_logger()->error("grant_write_access: open file failed for {}: {}", path.string(), strerror(errno));
                return false;
            }
        } else {
            utils::get_logger()->error("grant_write_access: open {} failed: {}", path.string(), strerror(errno));
            return false;
        }
    }

    struct stat st;
    if (fstat(ufd.get(), &st) != 0) {
        ufd.reset();
        utils::get_logger()->error("grant_write_access: fstat failed for {}: {}", path.string(), strerror(errno));
        return false;
    }

    mode_t new_mode = st.st_mode | (S_IRGRP | S_IWGRP | S_IXGRP);
    if (S_ISDIR(st.st_mode)) {
        new_mode |= S_IXGRP;
    }
    if (fchmod(ufd.get(), new_mode) != 0) {
        ufd.reset();
        utils::get_logger()->error("grant_write_access: fchmod failed for {}: {}", path.string(), strerror(errno));
        return false;
    }
    ufd.reset();

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

    utils::unique_fd ufd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    bool is_dir = true;
    if (!ufd) {
        if (errno == ELOOP) {
            utils::get_logger()->error("revoke_write_access: path is a symlink, rejecting: {}", path.string());
            return false;
        }
        if (errno == ENOTDIR) {
            is_dir = false;
            ufd.reset(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
            if (!ufd) {
                if (errno == ELOOP) {
                    utils::get_logger()->error("revoke_write_access: file path is symlink, rejecting: {}", path.string());
                    return false;
                }
                utils::get_logger()->warn("revoke_write_access: open file failed for {}: {}", path.string(), strerror(errno));
            }
        } else {
            utils::get_logger()->warn("revoke_write_access: open {} failed: {}", path.string(), strerror(errno));
        }
    }

    if (ufd) {
        struct stat st;
        if (fstat(ufd.get(), &st) == 0) {
            mode_t new_mode = st.st_mode;
            if (is_dir) {
                new_mode &= ~S_ISGID;
            }
            new_mode &= ~(S_IRGRP | S_IWGRP | S_IXGRP);
            if (fchmod(ufd.get(), new_mode) != 0) {
                utils::get_logger()->warn("revoke_write_access: fchmod failed for {}: {}", path.string(), strerror(errno));
            }
        }
        ufd.reset();
    }

    auto gpasswd_result = utils::exec_safe({"gpasswd", "-d", username, username});
    if (gpasswd_result.exit_code != 0) {
        utils::get_logger()->warn("gpasswd -d failed (user may not be in group): {}", gpasswd_result.stderr_output);
    }

    // Safety check: only delete group if it has no other members.  An attacker
    // could add themselves or another user to the ai-user's group before revoke,
    // causing groupdel to fail or remove a legitimate group used by others.
    // This prevents accidental DoS on shared groups.
    struct group* gr = getgrnam(username.c_str());
    if (!gr) {
        utils::get_logger()->info("Group '{}' does not exist, skipping groupdel", username);
    } else {
        bool has_others = false;
        for (char** mem = gr->gr_mem; *mem != nullptr; ++mem) {
            if (std::string(*mem) != username) {
                has_others = true;
                break;
            }
        }
        if (has_others) {
            utils::get_logger()->warn("Group '{}' has other members besides '{}', skipping groupdel to avoid DoS",
                username, username);
        } else {
            utils::exec_safe({"groupdel", username});
        }
    }

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

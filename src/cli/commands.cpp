#include "ai_mirror/cli/commands.hpp"
#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/core/config.hpp"
#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/daemon/health_check.hpp"
#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>

namespace fs = std::filesystem;

namespace ai_mirror::cli {

// Validates that ai_user belongs to main_user. The "_" separator after
// main_user prevents prefix collision (e.g. "alice" vs "alice_bob").
// The size > check ensures at least one project-name char follows the prefix.
static bool validate_ai_user_ownership(const std::string& ai_user, const std::string& main_user, const std::string& prefix) {
    if (ai_user.empty() || main_user.empty()) return false;
    std::string expected_prefix = prefix + main_user + "_";
    return ai_user.size() > expected_prefix.size()
        && ai_user.substr(0, expected_prefix.size()) == expected_prefix;
}

struct CommandContext {
    core::Config config;
    std::unique_ptr<core::UserManager> user_mgr;
    std::unique_ptr<core::Graft> graft;
    std::unique_ptr<core::SSHManager> ssh_mgr;
    bool verbose = false;
};

static CommandContext make_context(bool verbose) {
    CommandContext ctx;
    ctx.config = core::ConfigParser::load_default();
    ctx.user_mgr = std::make_unique<core::UserManager>(ctx.config.user.prefix, ctx.config.user.allowed_bases);
    ctx.graft = std::make_unique<core::Graft>(ctx.config.user.prefix);
    ctx.ssh_mgr = std::make_unique<core::SSHManager>();
    ctx.verbose = verbose;
    return ctx;
}

static int do_configure(CommandContext& ctx, const core::UserInfo& state,
                        const fs::path& proj, const std::string& main_user) {
    std::string username = state.username;
    std::string home_dir = state.home_dir;
    int fixes = 0;

    {
        fs::path mhome = utils::get_home_dir(main_user);
        fs::path p = fs::absolute(proj);
        std::vector<fs::path> dirs_to_fix;
        // 防止无限循环：最多遍历到根目录或最多32层
        int max_depth = 32;
        while (p.has_parent_path() && p != mhome && !p.empty() && p != "/" && --max_depth > 0) {
            p = p.parent_path();
            if (p == mhome || p == "/") break;
            dirs_to_fix.push_back(p);
        }
        for (auto& d : dirs_to_fix) {
            std::error_code ec;
            auto perms = fs::status(d, ec).permissions();
            if (ec) continue;
            if ((perms & fs::perms::group_exec) == fs::perms::none) {
                auto chgrp = utils::exec_safe({"chgrp", main_user, d.string()});
                if (chgrp.exit_code == 0) {
                    fs::permissions(d, perms | fs::perms::group_exec, ec);
                    if (!ec) {
                        utils::get_logger()->info("Added g+x to {} (group traverse for {})", d.string(), main_user);
                        fixes++;
                    }
                } else {
                    utils::get_logger()->warn("Failed to chgrp {} to {}: {}", d.string(), main_user, chgrp.stderr_output);
                }
            }
        }
        std::error_code iter_ec;
        for (const auto& entry : fs::directory_iterator(mhome, iter_ec)) {
            if (!entry.is_directory()) continue;
            std::error_code ec;
            auto ep = entry.status(ec).permissions();
            if (!ec && (ep & fs::perms::group_write) != fs::perms::none) {
                fs::permissions(entry.path(), ep & ~fs::perms::group_write, ec);
                if (!ec) {
                    utils::get_logger()->info("Removed g+w from {} (privacy protection)", entry.path().string());
                    fixes++;
                } else {
                    utils::get_logger()->warn("Failed to remove g+w from {}: {}", entry.path().string(), ec.message());
                }
            }
        }
    }

    {
        std::error_code ec;
        auto hp = fs::status(home_dir, ec);
        if (!ec && (hp.permissions() & (fs::perms::set_gid)) != fs::perms::none) {
            utils::get_logger()->info("Clearing setgid on {} (will be re-applied by grant_write_access if needed)", home_dir);
        }
    }

    {
        fs::path passwd_home = utils::get_home_dir(main_user);
        std::string env_home = utils::get_effective_home();
        if (!passwd_home.empty() && !env_home.empty() && passwd_home != env_home) {
            fs::path old_ssh = passwd_home / ".ssh";
            fs::path new_ssh = fs::path(env_home) / ".ssh";
            std::error_code ec;
            if (fs::exists(new_ssh) && !fs::exists(old_ssh, ec)) {
                fs::create_symlink(new_ssh, old_ssh, ec);
                if (!ec) {
                    struct passwd* main_pw = getpwnam(main_user.c_str());
                    if (!main_pw) {
                        utils::get_logger()->warn("Cannot resolve uid for main user '{}'", main_user);
                    } else {
                        auto chown_r = utils::exec_safe({"chown", "-h",
                            std::to_string(main_pw->pw_uid) + ":" + std::to_string(main_pw->pw_gid),
                            old_ssh.string()});
                        if (chown_r.exit_code == 0) {
                            utils::get_logger()->info("Created symlink {} -> {}", old_ssh.string(), new_ssh.string());
                            fixes++;
                        }
                    }
                }
            }
        }
    }

    auto grp_result = utils::exec_safe({"usermod", "-aG", main_user, username});
    if (grp_result.exit_code == 0) {
        fixes++;
    } else {
        utils::get_logger()->warn("Failed to add {} to {} group: {}", username, main_user, grp_result.stderr_output);
    }

    // Add main_user to ai_user's group for file access
    auto grp_result2 = utils::exec_safe({"usermod", "-aG", username, main_user});
    if (grp_result2.exit_code == 0) {
        fixes++;
        std::cout << "newgrp=" << username << std::endl;
    } else {
        utils::get_logger()->warn("Failed to add {} to {} group: {}", main_user, username, grp_result2.stderr_output);
    }

    ctx.ssh_mgr->set_key_path(ctx.config.ssh.key_path);
    ctx.ssh_mgr->set_key_type(ctx.config.ssh.key_type);

    // Check if authorized_keys exists AND contains current user's public key
    fs::path auth_keys = fs::path(home_dir) / ".ssh" / "authorized_keys";
    fs::path key_pub = fs::path(ctx.config.ssh.key_path.string() + ".pub");
    bool need_ssh_fix = false;
    if (!fs::exists(auth_keys)) {
        need_ssh_fix = true;
        utils::get_logger()->info("authorized_keys missing for {}", username);
    } else {
        // Check if authorized_keys contains the key_path's public key
        bool key_found = false;
        if (fs::exists(key_pub)) {
            std::ifstream pub_file(key_pub);
            std::string pub_key_line;
            if (std::getline(pub_file, pub_key_line) && !pub_key_line.empty()) {
                // Extract the base64 key body (second field) for exact matching
                auto first_space = pub_key_line.find(' ');
                auto second_space = (first_space != std::string::npos)
                    ? pub_key_line.find(' ', first_space + 1) : std::string::npos;
                std::string key_body = (first_space != std::string::npos && second_space != std::string::npos)
                    ? pub_key_line.substr(first_space + 1, second_space - first_space - 1)
                    : std::string();
                if (!key_body.empty()) {
                    std::ifstream auth_file(auth_keys);
                    std::string auth_line;
                    while (std::getline(auth_file, auth_line)) {
                        if (auth_line.find(key_body) != std::string::npos) {
                            key_found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!key_found) {
            need_ssh_fix = true;
            utils::get_logger()->info("authorized_keys exists but missing {}'s key, re-authorizing", main_user);
        }
    }

    if (need_ssh_fix) {
        utils::get_logger()->info("Fixing SSH: setup_passwordless for {}", username);
        if (ctx.ssh_mgr->setup_passwordless(main_user, username)) {
            fixes++;
        } else {
            utils::get_logger()->warn("Failed to fix SSH for {}", username);
        }
    }

    if (!ctx.config.ssh.ai_default_key.empty()) {
        ctx.ssh_mgr->setup_default_key_from_file(username, ctx.config.ssh.ai_default_key);
    }

    ctx.graft->invalidate_cache();
    auto existing = ctx.graft->list_mounts(username);

    std::set<std::string> configured_targets;
    for (const auto& mount_path : ctx.config.mount.paths) {
        auto source_opt = core::PathResolver::resolve(mount_path.string());
        if (!source_opt) continue;
        fs::path source = *source_opt;
        if (!fs::exists(source)) continue;
        fs::path target = core::PathResolver::to_ai_user_path(source, username, main_user, home_dir);
        configured_targets.insert(target.string());
    }

    for (const auto& m : existing) {
        if (!configured_targets.count(m.target.string())) {
            utils::get_logger()->info("Cleaning stale/duplicate mount: {}", m.target.string());
            auto umount_result = utils::exec_safe({"umount", m.target.string()});
            if (umount_result.exit_code != 0) {
                utils::get_logger()->warn("Failed to umount {}: {}", m.target.string(), umount_result.stderr_output);
            } else {
                fixes++;
            }
        }
    }

    int dup_cleaned = ctx.graft->cleanup_duplicate_mounts(username);
    if (dup_cleaned > 0) {
        fixes += dup_cleaned;
        utils::get_logger()->info("Cleaned {} duplicate mount(s) for {}", dup_cleaned, username);
    }

    int mount_failures = 0;
    for (const auto& mount_path : ctx.config.mount.paths) {
        auto source_opt = core::PathResolver::resolve(mount_path.string());
        if (!source_opt) continue;
        fs::path source = *source_opt;
        if (!fs::exists(source)) continue;
        if (!utils::is_path_allowed(source, main_user)) continue;

        fs::path target = core::PathResolver::to_ai_user_path(source, username, main_user, home_dir);
        if (ctx.graft->is_mounted(target)) continue;

        utils::get_logger()->info("Fixing mount: {} -> {}", source.string(), target.string());
        if (ctx.graft->bind_mount(source, target, true, state.uid, state.gid)) {
            fixes++;
        } else {
            mount_failures++;
            utils::get_logger()->warn("Failed to mount {} -> {}", source.string(), target.string());
        }
    }

    if (mount_failures > 0) {
        utils::get_logger()->warn("do_configure completed with {} mount failure(s)", mount_failures);
    }

    if (!ctx.graft->grant_write_access(proj, username)) {
        utils::get_logger()->warn("Failed to grant write access to: {}", proj.string());
    }

    // Fix SSH StrictModes compatibility: sshd requires the user's home directory
    // and ~/.ssh to NOT be group-writable. Since home_dir == proj and we just
    // set it to 775 (g+rwx) via grant_write_access, we must:
    //   1. Remove g+w from home_dir to satisfy sshd StrictModes
    //   2. Ensure .ssh directory is 700
    //   3. Ensure .ssh/authorized_keys is 600
    // The main user can still access files via group membership + setgid on
    // subdirectories created after newgrp.
    {
        std::error_code ec;
        auto hp = fs::status(home_dir, ec);
        if (!ec && (hp.permissions() & fs::perms::set_gid) != fs::perms::none) {
            fs::permissions(home_dir, hp.permissions() & ~fs::perms::set_gid, ec);
            if (!ec) {
                utils::get_logger()->info("Cleared setgid on home_dir {}", home_dir);
            }
        }
    }

    {
        // Remove g+w from home_dir for sshd StrictModes compatibility
        std::error_code ec;
        auto hp = fs::status(home_dir, ec);
        if (!ec && (hp.permissions() & fs::perms::group_write) != fs::perms::none) {
            fs::permissions(home_dir, hp.permissions() & ~fs::perms::group_write, ec);
            if (!ec) {
                utils::get_logger()->info("Removed g+w from home_dir {} (sshd StrictModes compatibility)", home_dir);
                fixes++;
            }
        }
    }

    {
        // Ensure .ssh is 700 and authorized_keys is 600, owned by ai-user
        // (sshd StrictModes requires correct ownership)
        fs::path ssh_dir = fs::path(home_dir) / ".ssh";
        fs::path auth_keys = ssh_dir / "authorized_keys";
        std::error_code ec;
        struct passwd* ai_pw = getpwnam(username.c_str());

        if (fs::exists(ssh_dir, ec) && !ec) {
            // Fix ownership
            if (ai_pw) {
                int ssh_fd = open(ssh_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                if (ssh_fd >= 0) {
                    struct stat st;
                    if (fstat(ssh_fd, &st) == 0 && (st.st_uid != ai_pw->pw_uid || st.st_gid != ai_pw->pw_gid)) {
                        if (fchown(ssh_fd, ai_pw->pw_uid, ai_pw->pw_gid) == 0) {
                            utils::get_logger()->info("Fixed .ssh ownership to {} (was uid={})", username, st.st_uid);
                            fixes++;
                        }
                    }
                    close(ssh_fd);
                }
            }
            // Fix permissions
            int ssh_fd = open(ssh_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (ssh_fd >= 0) {
                if (fchmod(ssh_fd, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
                    utils::get_logger()->warn("Failed to chmod .ssh to 700: {}", strerror(errno));
                } else {
                    utils::get_logger()->info("Set .ssh to 700 for StrictModes compatibility");
                    fixes++;
                }
                close(ssh_fd);
            }
        }

        if (fs::exists(auth_keys, ec) && !ec) {
            // Fix ownership
            if (ai_pw) {
                int ak_fd = open(auth_keys.c_str(), O_RDONLY | O_NOFOLLOW);
                if (ak_fd >= 0) {
                    struct stat st;
                    if (fstat(ak_fd, &st) == 0 && (st.st_uid != ai_pw->pw_uid || st.st_gid != ai_pw->pw_gid)) {
                        if (fchown(ak_fd, ai_pw->pw_uid, ai_pw->pw_gid) == 0) {
                            utils::get_logger()->info("Fixed authorized_keys ownership to {} (was uid={})", username, st.st_uid);
                            fixes++;
                        }
                    }
                    close(ak_fd);
                }
            }
            // Fix permissions
            int ak_fd = open(auth_keys.c_str(), O_RDONLY | O_NOFOLLOW);
            if (ak_fd >= 0) {
                if (fchmod(ak_fd, S_IRUSR | S_IWUSR) != 0) {
                    utils::get_logger()->warn("Failed to chmod authorized_keys to 600: {}", strerror(errno));
                } else {
                    utils::get_logger()->info("Set authorized_keys to 600 for StrictModes compatibility");
                    fixes++;
                }
                close(ak_fd);
            }
        }
    }

    utils::get_logger()->info("Configure complete: {} fix(es) applied for {}", fixes, username);
    return mount_failures > 0 ? 1 : 0;
}

int cmd_create(const std::string& project_path, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror create requires root privileges" << std::endl;
        return 1;
    }

    auto proj_opt = core::PathResolver::resolve(project_path);
    if (!proj_opt) {
        std::cerr << "Invalid project path: " << project_path << std::endl;
        return 1;
    }
    fs::path proj = *proj_opt;

    std::string main_user = utils::get_effective_username();
    if (!utils::is_path_allowed(proj, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Path not allowed: " << proj.string() << std::endl;
        return 1;
    }

    utils::get_logger()->info("Creating ai-user for project: {}", proj.string());

    auto user_info = ctx.user_mgr->create_ai_user(proj.string());
    if (!user_info.exists) {
        std::cerr << "Failed to create ai-user: " << user_info.error << std::endl;
        return 1;
    }

    int rc = do_configure(ctx, user_info, proj, main_user);
    if (rc != 0) {
        utils::get_logger()->warn("Create completed with configuration issues for {}", user_info.username);
    }

    std::cout << user_info.username << std::endl;
    return rc;
}

static bool safe_chown_file(const fs::path& p, const std::string& owner) {
    int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        utils::get_logger()->error("safe_chown_file: open({}) failed: {}", p.string(), strerror(errno));
        return false;
    }
    struct passwd* pw = getpwnam(owner.c_str());
    if (!pw) {
        close(fd);
        utils::get_logger()->error("safe_chown_file: user '{}' not found", owner);
        return false;
    }
    int ret = fchown(fd, pw->pw_uid, pw->pw_gid);
    close(fd);
    if (ret != 0) {
        utils::get_logger()->error("safe_chown_file: fchown({}) failed: {}", p.string(), strerror(errno));
        return false;
    }
    return true;
}

static bool safe_chown_single(const fs::path& p, uid_t uid, gid_t gid) {
    int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ELOOP) {
            if (lchown(p.c_str(), uid, gid) != 0) {
                utils::get_logger()->error("safe_chown: lchown({}) failed: {}", p.string(), strerror(errno));
                return false;
            }
            return true;
        }
        utils::get_logger()->error("safe_chown: open({}) failed: {}", p.string(), strerror(errno));
        return false;
    }
    int ret = fchown(fd, uid, gid);
    close(fd);
    if (ret != 0) {
        utils::get_logger()->error("safe_chown: fchown({}) failed: {}", p.string(), strerror(errno));
        return false;
    }
    return true;
}

// FD-based recursive chown using openat()+fchownat() with O_NOFOLLOW.
// Opens each directory by fd to prevent TOCTOU symlink injection during
// traversal: an attacker cannot inject a symlink that redirects the walk
// outside the intended subtree because every component is opened relative
// to its parent directory fd with O_NOFOLLOW (symlinks fail with ELOOP).
// Symlinks at the leaf are handled with lchown (change the link itself).
static bool chown_recursive_fd(int dirfd, uid_t uid, gid_t gid, int depth = 0) {
    constexpr int max_depth = 1000;
    if (depth >= max_depth) {
        utils::get_logger()->error("chown_recursive_fd: max depth {} exceeded", max_depth);
        return false;
    }

    DIR* d = fdopendir(dirfd);
    if (!d) {
        utils::get_logger()->error("safe_chown_path: fdopendir failed: {}", strerror(errno));
        close(dirfd);
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        int fd = -1;
        int retries = 0;
        while (retries < 3) {
            fd = openat(dirfd, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0 || errno != EINTR) break;
            retries++;
        }
        if (fd < 0) {
            if (errno == ELOOP) {
                if (fchownat(dirfd, entry->d_name, uid, gid, AT_SYMLINK_NOFOLLOW) != 0) {
                    utils::get_logger()->warn("safe_chown_path: lchown {} failed: {}", entry->d_name, strerror(errno));
                }
                continue;
            }
            utils::get_logger()->warn("safe_chown_path: openat {} failed: {}", entry->d_name, strerror(errno));
            continue;
        }

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            utils::get_logger()->warn("safe_chown_path: fstat {} failed: {}", entry->d_name, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!chown_recursive_fd(fd, uid, gid, depth + 1)) {
                closedir(d);
                return false;
            }
        } else {
            if (fchown(fd, uid, gid) != 0) {
                utils::get_logger()->warn("safe_chown_path: fchown {} failed: {}", entry->d_name, strerror(errno));
            }
            close(fd);
        }
    }

    closedir(d);
    return true;
}

// FD-based recursive chmod to strip setuid/setgid bits without following symlinks.
// Uses fchmodat(AT_SYMLINK_NOFOLLOW) so symlinks are chmod'ed directly,
// preventing symlink traversal attacks.  Regular files/dirs use fchmod.
static bool chmod_recursive_fd(int dirfd, mode_t clear_bits) {
    DIR* d = fdopendir(dirfd);
    if (!d) {
        utils::get_logger()->error("chmod_recursive_fd: fdopendir failed: {}", strerror(errno));
        close(dirfd);
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        int fd = -1;
        int retries = 0;
        while (retries < 3) {
            fd = openat(dirfd, entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0 || errno != EINTR) break;
            retries++;
        }
        if (fd < 0) {
            if (errno == ELOOP) {
                mode_t link_mode = 0777;
                struct stat link_st;
                if (fstatat(dirfd, entry->d_name, &link_st, AT_SYMLINK_NOFOLLOW) == 0) {
                    link_mode = link_st.st_mode;
                }
                mode_t new_mode = link_mode & ~clear_bits;
                if (fchmodat(dirfd, entry->d_name, new_mode, AT_SYMLINK_NOFOLLOW) != 0) {
                    utils::get_logger()->warn("chmod_recursive_fd: fchmodat symlink {} failed: {}", entry->d_name, strerror(errno));
                }
                continue;
            }
            if (errno == ENOTDIR || errno == ENOENT) {
                continue;
            }
            utils::get_logger()->warn("chmod_recursive_fd: openat {} failed: {}", entry->d_name, strerror(errno));
            continue;
        }

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            utils::get_logger()->warn("chmod_recursive_fd: fstat {} failed: {}", entry->d_name, strerror(errno));
            continue;
        }

        mode_t new_mode = st.st_mode & ~clear_bits;
        if (S_ISDIR(st.st_mode)) {
            if (fchmod(fd, new_mode) != 0) {
                utils::get_logger()->warn("chmod_recursive_fd: fchmod dir {} failed: {}", entry->d_name, strerror(errno));
            }
            if (!chmod_recursive_fd(fd, clear_bits)) {
                closedir(d);
                return false;
            }
        } else {
            if (fchmod(fd, new_mode) != 0) {
                utils::get_logger()->warn("chmod_recursive_fd: fchmod {} failed: {}", entry->d_name, strerror(errno));
            }
            close(fd);
        }
    }

    closedir(d);
    return true;
}

// Recursively changes ownership of a path using fd-based traversal.
// Uses O_NOFOLLOW at every level to detect and skip symlinks, preventing
// TOCTOU attacks where an attacker replaces a directory entry with a symlink
// pointing outside the subtree between readdir() and chown().  The root
// directory is also opened with O_NOFOLLOW so the entry point itself cannot
// be a symlink.  After ownership transfer, setuid/setgid bits are stripped.
static bool safe_chown_path(const fs::path& p, const std::string& owner) {
    struct passwd* pw = getpwnam(owner.c_str());
    if (!pw) {
        utils::get_logger()->error("safe_chown_path: user '{}' not found", owner);
        return false;
    }

    int rootfd = open(p.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (rootfd < 0) {
        if (errno == ENOTDIR || errno == ELOOP) {
            return safe_chown_single(p, pw->pw_uid, pw->pw_gid);
        }
        utils::get_logger()->error("safe_chown_path: open({}) failed: {}", p.string(), strerror(errno));
        return false;
    }

    if (!chown_recursive_fd(rootfd, pw->pw_uid, pw->pw_gid)) {
        return false;
    }

    int topfd = open(p.c_str(), O_RDONLY | O_DIRECTORY);
    if (topfd >= 0) {
        if (fchown(topfd, pw->pw_uid, pw->pw_gid) != 0) {
            utils::get_logger()->error("safe_chown_path: fchown root {} failed: {}", p.string(), strerror(errno));
            close(topfd);
            return false;
        }
        close(topfd);
    }

    int chmodfd = open(p.c_str(), O_RDONLY | O_DIRECTORY);
    if (chmodfd >= 0) {
        mode_t clear_bits = S_ISUID | S_ISGID;
        struct stat root_st;
        if (fstat(chmodfd, &root_st) == 0) {
            mode_t new_root_mode = root_st.st_mode & ~clear_bits;
            if (fchmod(chmodfd, new_root_mode) != 0) {
                utils::get_logger()->warn("safe_chown_path: fchmod root {} failed: {}", p.string(), strerror(errno));
            }
        }
        if (!chmod_recursive_fd(chmodfd, clear_bits)) {
            utils::get_logger()->warn("safe_chown_path: chmod_recursive_fd failed for {}", p.string());
        }
    }
    return true;
}

int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror mkdir requires root privileges" << std::endl;
        return 1;
    }

    if (!utils::validate_username(ai_user)) {
        std::cerr << "Invalid ai_user name: " << ai_user << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();
    if (!validate_ai_user_ownership(ai_user, main_user, ctx.config.user.prefix)) {
        std::cerr << "ai_user '" << ai_user << "' does not belong to user '" << main_user << "'" << std::endl;
        return 1;
    }

    auto dir_path_opt = core::PathResolver::resolve(path);
    if (!dir_path_opt) {
        std::cerr << "Invalid path: " << path << std::endl;
        return 1;
    }
    fs::path dir_path = *dir_path_opt;

    if (!utils::is_path_allowed(dir_path, main_user)) {
        std::cerr << "Path not allowed: " << dir_path.string() << std::endl;
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(dir_path, ec)) {
        if (!security::safe_create_directories(dir_path)) {
            std::cerr << "Failed to create directory: " << dir_path.string() << std::endl;
            return 1;
        }
    }

    if (!ctx.graft->grant_write_access(dir_path, ai_user)) {
        std::cerr << "Failed to grant write access" << std::endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Granted write access: " << dir_path.string() << " -> " << ai_user << std::endl;
    }
    return 0;
}

int cmd_touch(const std::string& path, const std::string& ai_user, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror touch requires root privileges" << std::endl;
        return 1;
    }

    if (!utils::validate_username(ai_user)) {
        std::cerr << "Invalid ai_user name: " << ai_user << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();
    if (!validate_ai_user_ownership(ai_user, main_user, ctx.config.user.prefix)) {
        std::cerr << "ai_user '" << ai_user << "' does not belong to user '" << main_user << "'" << std::endl;
        return 1;
    }

    auto file_path_opt = core::PathResolver::resolve(path);
    if (!file_path_opt) {
        std::cerr << "Invalid path: " << path << std::endl;
        return 1;
    }
    fs::path file_path = *file_path_opt;

    if (!utils::is_path_allowed(file_path, main_user)) {
        std::cerr << "Path not allowed: " << file_path.string() << std::endl;
        return 1;
    }

    if (!fs::exists(file_path)) {
        std::error_code ec;
        fs::path parent = file_path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            if (!utils::is_path_allowed(parent, main_user)) {
                std::cerr << "Parent path not allowed: " << parent.string() << std::endl;
                return 1;
            }
            if (!security::safe_create_directories(parent)) {
                std::cerr << "Failed to create parent directory: " << parent.string() << std::endl;
                return 1;
            }
        }
        int fd = open(file_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            std::cerr << "Failed to create file: " << file_path.string() << std::endl;
            return 1;
        }
        close(fd);
    }

    if (!safe_chown_file(file_path, ai_user)) {
        std::cerr << "Failed to set ownership for " << ai_user << std::endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Created file: " << file_path.string() << " (owner: " << ai_user << ")" << std::endl;
    }
    return 0;
}

int cmd_cp(const std::string& src, const std::string& dst, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror cp requires root privileges" << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();

    auto src_path_opt = core::PathResolver::resolve(src);
    if (!src_path_opt || !fs::exists(*src_path_opt)) {
        std::cerr << "Source does not exist: " << src << std::endl;
        return 1;
    }
    fs::path src_path = *src_path_opt;

    auto dst_path_opt = core::PathResolver::resolve(dst);
    if (!dst_path_opt) {
        std::cerr << "Invalid destination path: " << dst << std::endl;
        return 1;
    }
    fs::path dst_path = *dst_path_opt;

    if (!utils::is_path_allowed(dst_path, main_user)) {
        std::cerr << "Destination path not allowed: " << dst_path.string() << std::endl;
        return 1;
    }

    if (!utils::is_path_allowed(src_path, main_user)) {
        std::cerr << "Source path not allowed: " << src_path.string() << std::endl;
        return 1;
    }

    std::string ai_user = core::PathResolver::detect_ai_user_from_path(dst_path, main_user, ctx.config.user.prefix);
    if (ai_user.empty()) {
        std::cerr << "Warning: destination '" << dst_path.string()
                  << "' is not under any ai-user directory. Ownership will not be set."
                  << std::endl;
        std::cerr << "Consider using the regular 'cp' command for non-ai-user destinations." << std::endl;
        auto cp_result = utils::exec_safe({"cp", "-rP", "--no-preserve=mode", src_path.string(), dst_path.string()});
        if (cp_result.exit_code != 0) {
            std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
            return 1;
        }
        if (verbose) {
            std::cout << "Copied: " << src_path.string() << " -> " << dst_path.string() << std::endl;
        }
        return 0;
    }

    if (!validate_ai_user_ownership(ai_user, main_user, ctx.config.user.prefix)) {
        std::cerr << "ai_user '" << ai_user << "' does not belong to user '" << main_user << "'" << std::endl;
        return 1;
    }

    mode_t old_umask = umask(0077);
    auto cp_result = utils::exec_safe({"cp", "-rP", "--no-preserve=mode", src_path.string(), dst_path.string()});
    if (cp_result.exit_code != 0) {
        umask(old_umask);
        std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
        return 1;
    }

    fs::path chown_target = fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
    if (!safe_chown_path(chown_target, ai_user)) {
        umask(old_umask);
        std::cerr << "Failed to set ownership for " << ai_user << std::endl;
        return 1;
    }
    umask(old_umask);

    if (verbose) {
        std::cout << "Copied: " << src_path.string() << " -> " << dst_path.string() << " (owner: " << ai_user << ")" << std::endl;
    }
    return 0;
}

int cmd_mv(const std::string& src, const std::string& dst, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror mv requires root privileges" << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();

    auto src_path_opt = core::PathResolver::resolve(src);
    if (!src_path_opt || !fs::exists(*src_path_opt)) {
        std::cerr << "Source does not exist: " << src << std::endl;
        return 1;
    }
    fs::path src_path = *src_path_opt;

    auto dst_path_opt = core::PathResolver::resolve(dst);
    if (!dst_path_opt) {
        std::cerr << "Invalid destination path: " << dst << std::endl;
        return 1;
    }
    fs::path dst_path = *dst_path_opt;

    if (!utils::is_path_allowed(dst_path, main_user)) {
        std::cerr << "Destination path not allowed: " << dst_path.string() << std::endl;
        return 1;
    }

    if (!utils::is_path_allowed(src_path, main_user)) {
        std::cerr << "Source path not allowed: " << src_path.string() << std::endl;
        return 1;
    }

    std::string ai_user = core::PathResolver::detect_ai_user_from_path(dst_path, main_user, ctx.config.user.prefix);
    bool need_chown = !ai_user.empty();

    if (need_chown) {
        if (!validate_ai_user_ownership(ai_user, main_user, ctx.config.user.prefix)) {
            std::cerr << "ai_user '" << ai_user << "' does not belong to user '" << main_user << "'" << std::endl;
            return 1;
        }
    }

    std::error_code ec;
    fs::rename(src_path, dst_path, ec);
    if (ec) {
        auto cp_result = utils::exec_safe({"cp", "-rP", "--no-preserve=mode", src_path.string(), dst_path.string()});
        if (cp_result.exit_code != 0) {
            std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
            return 1;
        }

        if (need_chown) {
            fs::path chown_target = fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
            if (!safe_chown_path(chown_target, ai_user)) {
                std::cerr << "Failed to set ownership after copy" << std::endl;
                return 1;
            }
        }

        struct stat src_stat;
        if (lstat(src_path.c_str(), &src_stat) != 0) {
            utils::get_logger()->warn("Failed to stat source after copy: {}", strerror(errno));
        } else if (S_ISLNK(src_stat.st_mode)) {
            if (unlink(src_path.c_str()) != 0) {
                utils::get_logger()->warn("Failed to unlink symlink source: {}", strerror(errno));
            }
        } else if (S_ISDIR(src_stat.st_mode)) {
            int srcfd = open(src_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (srcfd < 0) {
                if (errno == ELOOP) {
                    utils::get_logger()->warn("Source became symlink, refusing recursive delete: {}", src_path.string());
                } else {
                    fs::remove_all(src_path, ec);
                    if (ec) {
                        utils::get_logger()->warn("Failed to remove source directory: {}", ec.message());
                    }
                }
            } else {
                auto remove_result = fs::remove_all(src_path);
                if (remove_result == static_cast<std::uintmax_t>(-1)) {
                    utils::get_logger()->warn("Failed to remove source directory");
                }
                close(srcfd);
            }
        } else {
            if (unlink(src_path.c_str()) != 0) {
                utils::get_logger()->warn("Failed to remove source file: {}", strerror(errno));
            }
        }

        if (verbose) {
            if (need_chown) {
                std::cout << "Moved (copy+delete): " << src_path.string() << " -> " << dst_path.string() << " (owner: " << ai_user << ")" << std::endl;
            } else {
                std::cout << "Moved (copy+delete): " << src_path.string() << " -> " << dst_path.string() << std::endl;
            }
        }
        return 0;
    }

    if (need_chown) {
        fs::path chown_target = fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
        if (!safe_chown_path(chown_target, ai_user)) {
            utils::get_logger()->error("Rename succeeded but chown failed for {}, attempting rollback", chown_target.string());
            std::error_code rollback_ec;
            for (int retry = 0; retry < 3; ++retry) {
                fs::rename(dst_path, src_path, rollback_ec);
                if (!rollback_ec) break;
            }
            if (rollback_ec) {
                utils::get_logger()->error("Rollback failed after 3 retries: {} - MANUAL INTERVENTION REQUIRED for {}", rollback_ec.message(), dst_path.string());
            }
            std::cerr << "Failed to set ownership after atomic rename" << std::endl;
            return 1;
        }
    }

    if (verbose) {
        if (need_chown) {
            std::cout << "Moved (atomic): " << src_path.string() << " -> " << dst_path.string() << " (owner: " << ai_user << ")" << std::endl;
        } else {
            std::cout << "Moved (atomic): " << src_path.string() << " -> " << dst_path.string() << std::endl;
        }
    }
    return 0;
}

int cmd_cd(const std::string& path, [[maybe_unused]] bool verbose) {
    auto config = core::ConfigParser::load_default();
    std::string prefix = config.user.prefix;

    std::string main_user = utils::get_effective_username();

    auto target_opt = core::PathResolver::resolve(path);
    if (!target_opt) {
        std::cerr << "Cannot resolve path: " << path << std::endl;
        return 1;
    }
    fs::path target = *target_opt;
    if (!fs::exists(target)) {
        std::cerr << "Path does not exist: " << path << std::endl;
        return 1;
    }

    std::string target_str = target.string();
    if (!utils::validate_path_no_shell_metachars(target_str)) {
        std::cerr << "Path contains disallowed characters" << std::endl;
        return 1;
    }

    if (target_str == "/") {
        std::cerr << "Path traversal detected: resolves to root directory" << std::endl;
        return 1;
    }

    if (!security::validate_mount_source(target)) {
        std::cerr << "Path not in allowed directory: " << target_str << std::endl;
        return 1;
    }

    // Primary method: find .am_status by walking up from target directory
    // This works for any filesystem (BeeGFS, NFS, local) regardless of home_dir location
    fs::path search_path = target;
    std::optional<core::UserInfo> state;
    std::error_code ec;
    
    while (!search_path.empty() && search_path != "/" && search_path != "/home") {
        state = core::UserManager::read_state(search_path);
        if (state) break;
        search_path = search_path.parent_path();
    }

    if (state) {
        std::string ai_user = state->username;
        
        // Health check
        auto graft = core::Graft(prefix);
        auto mounts = graft.list_mounts(ai_user);
        bool has_broken = false;
        std::set<std::string> mounted_targets;
        for (const auto& m : mounts) {
            mounted_targets.insert(m.target.string());
            if (!m.active) has_broken = true;
        }

        bool missing_ssh = !fs::exists(fs::path(state->home_dir) / ".ssh" / "authorized_keys", ec);

        size_t expected_mounts = 0;
        for (const auto& mp : config.mount.paths) {
            auto src = core::PathResolver::resolve(mp.string());
            if (src && fs::exists(*src)) {
                expected_mounts++;
                fs::path tgt = core::PathResolver::to_ai_user_path(*src, ai_user, main_user, state->home_dir);
                if (!mounted_targets.count(tgt.string())) has_broken = true;
            }
        }

        if (has_broken || missing_ssh) {
            std::cerr << "WARNING: project health issues detected, run 'am update " << target_str << "' to fix:" << std::endl;
            if (missing_ssh) std::cerr << "  - SSH authorized_keys missing" << std::endl;
            if (has_broken && expected_mounts > 0) std::cerr << "  - mounts broken or missing" << std::endl;
        }

        std::cout << "action=ssh" << std::endl;
        std::cout << "user=" << ai_user << std::endl;
        std::cout << "path=" << target_str << std::endl;
        std::cout << "key=" << config.ssh.key_path.string() << std::endl;

        // Debug line for SSH troubleshooting
        {
            fs::path ssh_dir = fs::path(state->home_dir) / ".ssh";
            fs::path auth_keys = ssh_dir / "authorized_keys";
            bool ssh_dir_exists = fs::exists(ssh_dir, ec) && !ec;
            bool auth_keys_exists = fs::exists(auth_keys, ec) && !ec;
            std::string ssh_dir_perms = "missing";
            std::string auth_keys_perms = "missing";
            if (ssh_dir_exists) {
                auto st = fs::status(ssh_dir, ec);
                if (!ec) ssh_dir_perms = std::to_string(static_cast<unsigned>(st.permissions() & fs::perms::all));
            }
            if (auth_keys_exists) {
                auto st = fs::status(auth_keys, ec);
                if (!ec) auth_keys_perms = std::to_string(static_cast<unsigned>(st.permissions() & fs::perms::all));
            }
            // Check if main user's public key is in authorized_keys
            bool key_authorized = false;
            if (auth_keys_exists) {
                std::ifstream ak(auth_keys);
                // Use configured key_path to derive public key, not hardcoded id_ed25519
                fs::path main_pub = fs::path(config.ssh.key_path.string() + ".pub");
                std::ifstream pk(main_pub);
                if (pk.is_open()) {
                    std::string pub_key_line, auth_line;
                    std::getline(pk, pub_key_line);
                    auto space = pub_key_line.find(' ');
                    if (space != std::string::npos) {
                        std::string key_type = pub_key_line.substr(0, space);
                        while (std::getline(ak, auth_line)) {
                            if (auth_line.find(key_type) != std::string::npos) {
                                key_authorized = true;
                                break;
                            }
                        }
                    }
                }
            }
            std::cout << "debug=home=" << state->home_dir
                      << ",ssh_perms=" << ssh_dir_perms
                      << ",auth_perms=" << auth_keys_perms
                      << ",key_in_auth=" << (key_authorized ? "yes" : "no")
                      << std::endl;
        }

        return 0;
    }

    // Fallback: detect AI user from path component name (legacy method)
    std::string ai_user = core::PathResolver::detect_ai_user_from_path(target, main_user, prefix);

    if (!ai_user.empty()) {
        std::cout << "action=ssh" << std::endl;
        std::cout << "user=" << ai_user << std::endl;
        std::cout << "path=" << target_str << std::endl;
        return 0;
    }

    std::cout << "action=cd" << std::endl;
    std::cout << "path=" << target_str << std::endl;
    return 0;
}

int cmd_list(bool verbose) {
    auto ctx = make_context(verbose);
    auto users = ctx.user_mgr->list_ai_users();

    if (users.empty()) {
        std::cout << "No ai-mirror managed users found." << std::endl;
        return 0;
    }

    std::string main_user = utils::get_effective_username();
    std::string expected_prefix = ctx.config.user.prefix + main_user + "_";

    std::cout << "ai-mirror managed users:" << std::endl;
    for (const auto& u : users) {
        if (u.username.substr(0, expected_prefix.length()) != expected_prefix) continue;
        std::cout << "  " << u.username << " (uid=" << u.uid << ", home=" << u.home_dir << ")" << std::endl;
        auto mounts = ctx.graft->list_mounts(u.username);
        for (const auto& m : mounts) {
            std::cout << "    mount: " << m.source.string() << " -> " << m.target.string()
                      << (m.read_only ? " (ro)" : " (rw)") << std::endl;
        }
    }
    return 0;
}

int cmd_health([[maybe_unused]] bool verbose) {
    auto ctx = make_context(verbose);
    daemon::HealthCheck hc(ctx.config.user.prefix);
    auto statuses = hc.check_all();

    if (statuses.empty()) {
        std::cout << "No mounts to check." << std::endl;
        return 0;
    }

    int unhealthy = 0;
    for (const auto& s : statuses) {
        std::string status = s.healthy ? "OK" : "FAIL";
        std::cout << "[" << status << "] " << s.mount_point << " - " << s.detail << std::endl;
        if (!s.healthy) unhealthy++;
    }

    return unhealthy > 0 ? 1 : 0;
}

int cmd_force_destroy(const std::string& project_or_user, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror force-destroy requires root privileges" << std::endl;
        return 1;
    }

    std::string username = project_or_user;
    if (!utils::validate_username(username)) {
        auto derived = ctx.user_mgr->derive_username(project_or_user);
        if (!derived) {
            std::cerr << "Cannot derive valid username for: " << project_or_user << std::endl;
            return 1;
        }
        username = std::move(*derived);
    }
    if (!ctx.user_mgr->user_exists(username)) {
        std::cerr << "User not found: " << username << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();
    if (!validate_ai_user_ownership(username, main_user, ctx.config.user.prefix)) {
        std::cerr << "User '" << username << "' does not belong to '" << main_user << "'" << std::endl;
        return 1;
    }

    utils::get_logger()->warn("Force destroying user: {}", username);

    daemon::MountCleaner cleaner(ctx.config.user.prefix);
    cleaner.cleanup_for_user(username);

    if (!ctx.user_mgr->remove_ai_user(username, true)) {
        std::cerr << "Failed to remove user: " << username << std::endl;
        return 1;
    }

    std::cout << "Destroyed: " << username << std::endl;
    return 0;
}

static void terminate_user_processes(const std::string& username) {
    auto result = utils::exec_safe({"pkill", "-u", username});
    if (result.exit_code == 0) {
        utils::get_logger()->info("Terminated processes for user {}", username);
        usleep(500000);
    }
}

int cmd_rm(const std::string& project_path, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror rm requires root privileges" << std::endl;
        return 1;
    }

    auto proj_opt = core::PathResolver::resolve(project_path);
    if (!proj_opt) {
        std::cerr << "Invalid project path: " << project_path << std::endl;
        return 1;
    }
    fs::path proj = *proj_opt;

    auto derived = ctx.user_mgr->derive_username(proj.string());
    if (!derived) {
        std::cerr << "Username collision: cannot derive unique username for: " << proj.string() << std::endl;
        return 1;
    }
    std::string username = std::move(*derived);

    std::string main_user = utils::get_effective_username();
    if (!validate_ai_user_ownership(username, main_user, ctx.config.user.prefix)) {
        std::cerr << "ai_user '" << username << "' does not belong to user '" << main_user << "'" << std::endl;
        return 1;
    }

    if (!ctx.user_mgr->user_exists(username)) {
        std::cerr << "AI user not found for project: " << proj.string() << std::endl;
        std::cerr << "Expected user: " << username << std::endl;
        return 1;
    }

    auto user_info = ctx.user_mgr->get_user_info(username);
    if (!user_info) {
        std::cerr << "Failed to get user info: " << username << std::endl;
        return 1;
    }

    fs::path ai_home(user_info->home_dir);

    utils::get_logger()->info("Removing project: {} (user: {})", proj.string(), username);

    if (verbose) {
        std::cout << "Step 1: Unmounting bind mounts for " << username << std::endl;
    }
    daemon::MountCleaner cleaner(ctx.config.user.prefix);
    cleaner.cleanup_for_user(username);

    terminate_user_processes(username);

    if (verbose) {
        std::cout << "Step 2: Removing user " << username << std::endl;
    }
    if (!ctx.user_mgr->remove_ai_user(username, false)) {
        std::cerr << "Failed to remove user: " << username << std::endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Step 3: Cleaning up ai-user home" << std::endl;
    }
    {
        std::error_code ec;
        fs::remove_all(ai_home, ec);
        if (ec) {
            utils::get_logger()->warn("Failed to clean home dir: {}", ec.message());
        }
    }

    if (verbose) {
        std::cout << "Step 4: Revoking write grants on project" << std::endl;
    }
    if (!ctx.graft->revoke_write_access(proj, username)) {
        utils::get_logger()->warn("Failed to revoke write access for user '{}' on project '{}'", username, proj.string());
    }

    std::cout << "Removed: " << username << std::endl;
    return 0;
}

int cmd_config([[maybe_unused]] bool verbose) {
    auto config = core::ConfigParser::load_default();
    std::cout << "Config file: " << config.config_path.string() << std::endl;
    std::cout << "User prefix: " << config.user.prefix << " (system-level)" << std::endl;
    std::cout << "SSH key type: " << config.ssh.key_type << std::endl;
    std::cout << "SSH key path: " << config.ssh.key_path.string() << std::endl;
    std::cout << "AI default key: " << config.ssh.ai_default_key.string() << std::endl;
    std::cout << "Mount paths:" << std::endl;
    for (const auto& p : config.mount.paths) {
        std::cout << "  - " << p.string() << std::endl;
    }
    std::cout << "Loaded: " << (config.loaded ? "yes" : "no (using defaults)") << std::endl;
    return 0;
}

int cmd_status([[maybe_unused]] bool verbose) {
    auto ctx = make_context(verbose);
    auto users = ctx.user_mgr->list_ai_users();

    if (users.empty()) {
        std::cout << "No ai-mirror managed projects." << std::endl;
        return 0;
    }

    std::string main_user = utils::get_effective_username();
    std::string expected_prefix = ctx.config.user.prefix + main_user + "_";

    for (const auto& u : users) {
        if (u.username.substr(0, expected_prefix.length()) != expected_prefix) continue;
        std::cout << "Project: " << u.username << std::endl;
        std::cout << "  Home: " << u.home_dir << std::endl;
        std::cout << "  UID:  " << u.uid << std::endl;

        bool all_healthy = true;

        auto mounts = ctx.graft->list_mounts(u.username);
        if (mounts.empty()) {
            std::cout << "  Mounts: none" << std::endl;
        } else {
            std::cout << "  Mounts:" << std::endl;
            for (const auto& m : mounts) {
                std::string state = m.active ? "active" : "broken";
                std::string mode = m.read_only ? "ro" : "rw";
                std::cout << "    " << m.source.string() << " -> " << m.target.string()
                          << " (" << mode << ", " << state << ")" << std::endl;
                if (!m.active) all_healthy = false;
            }
        }

        fs::path key_path = ctx.config.ssh.key_path;
        if (key_path.string().size() >= 2 && key_path.string()[0] == '~' && key_path.string()[1] == '/') {
            key_path = fs::path(utils::get_effective_home()) / key_path.string().substr(2);
        }
        bool ssh_ok = fs::exists(key_path) && fs::exists(fs::path(key_path.string() + ".pub"));
        std::cout << "  SSH:   " << (ssh_ok ? "ok" : "missing") << std::endl;
        if (!ssh_ok) all_healthy = false;

        fs::path auth_keys = fs::path(u.home_dir) / ".ssh" / "authorized_keys";
        std::cout << "  Auth:  " << (fs::exists(auth_keys) ? "ok" : "missing") << std::endl;
        if (!fs::exists(auth_keys)) all_healthy = false;

        if (mounts.empty()) all_healthy = false;

        std::cout << "  Status: " << (all_healthy ? "healthy" : "unhealthy") << std::endl;
        std::cout << std::endl;
    }

    return 0;
}

int cmd_update(const std::string& path, [[maybe_unused]] bool verbose) {
    auto ctx = make_context(verbose);

    auto target_opt = core::PathResolver::resolve(path);
    if (!target_opt) {
        std::cerr << "Cannot resolve path: " << path << std::endl;
        return 1;
    }
    fs::path proj = *target_opt;
    if (!fs::exists(proj)) {
        std::cerr << "Path does not exist: " << path << std::endl;
        return 1;
    }

    auto state = core::UserManager::read_state(proj);
    if (!state) {
        std::cerr << "No .am_status found in: " << proj.string() << std::endl;
        return 1;
    }

    std::string main_user = state->main_user.empty()
        ? utils::get_effective_username()
        : state->main_user;

    return do_configure(ctx, *state, proj, main_user);
}

}

#include "ai_mirror/cli/commands.hpp"
#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/core/config.hpp"
#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/daemon/health_check.hpp"
#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/daemon/watch_stats.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>

// FTXUI for watch TUI
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <pthread.h>

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

    // Supplementary groups from [ai-user] config
    // Security rules:
    //   1. ai-mirror group is ALWAYS rejected (ai-user must never be in ai-mirror)
    //   2. If main_user is not a member of the requested group, reject (no privilege escalation)
    //   3. If the group does not exist on the system, warn and skip
    for (const auto& group_name : ctx.config.ai_user.groups) {
        // Rule 1: Never allow ai-mirror group
        if (group_name == "ai-mirror") {
            std::cerr << "SECURITY: refusing to add ai-user to 'ai-mirror' group" << std::endl;
            continue;
        }

        // Check group exists
        struct group* grp = getgrnam(group_name.c_str());
        if (!grp) {
            std::cerr << "WARNING: group '" << group_name << "' does not exist, skipping" << std::endl;
            continue;
        }

        // Rule 2: main_user must be a member of the group
        struct passwd* main_pw = getpwnam(main_user.c_str());
        if (!main_pw) {
            std::cerr << "WARNING: cannot resolve main user '" << main_user << "' for group check" << std::endl;
            continue;
        }

        bool main_in_group = false;
        // Check primary group
        if (main_pw->pw_gid == grp->gr_gid) {
            main_in_group = true;
        }
        // Check supplementary groups
        if (!main_in_group) {
            int ngroups = 0;
            getgrouplist(main_user.c_str(), main_pw->pw_gid, nullptr, &ngroups);
            if (ngroups > 0) {
                std::vector<gid_t> groups(ngroups);
                if (getgrouplist(main_user.c_str(), main_pw->pw_gid, groups.data(), &ngroups) >= 0) {
                    for (int i = 0; i < ngroups; ++i) {
                        if (groups[i] == grp->gr_gid) {
                            main_in_group = true;
                            break;
                        }
                    }
                }
            }
        }

        if (!main_in_group) {
            std::cerr << "SECURITY: refusing to add ai-user to '" << group_name
                      << "' group (main user '" << main_user << "' is not a member)" << std::endl;
            continue;
        }

        // Check if ai-user is already in the group
        bool ai_in_group = false;
        struct passwd* ai_pw = getpwnam(username.c_str());
        if (ai_pw) {
            int ai_ngroups = 0;
            getgrouplist(username.c_str(), ai_pw->pw_gid, nullptr, &ai_ngroups);
            if (ai_ngroups > 0) {
                std::vector<gid_t> ai_groups(ai_ngroups);
                if (getgrouplist(username.c_str(), ai_pw->pw_gid, ai_groups.data(), &ai_ngroups) >= 0) {
                    for (int i = 0; i < ai_ngroups; ++i) {
                        if (ai_groups[i] == grp->gr_gid) {
                            ai_in_group = true;
                            break;
                        }
                    }
                }
            }
        }

        if (ai_in_group) {
            utils::get_logger()->info("ai-user '{}' already in group '{}'", username, group_name);
            continue;
        }

        auto supp_result = utils::exec_safe({"usermod", "-aG", group_name, username});
        if (supp_result.exit_code == 0) {
            fixes++;
            utils::get_logger()->info("Added ai-user '{}' to supplementary group '{}'", username, group_name);
        } else {
            std::cerr << "WARNING: failed to add ai-user to '" << group_name << "' group: "
                      << supp_result.stderr_output << std::endl;
        }
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

    // Sync known_hosts from main_user so AI user can SSH to known hosts
    ctx.ssh_mgr->sync_known_hosts(main_user, username);

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
        if (!utils::is_path_allowed(source, main_user, ctx.config.user.allowed_bases)) continue;

        fs::path target = core::PathResolver::to_ai_user_path(source, username, main_user, home_dir);

        // Check if target is mounted and whether it's stale
        // A bind mount becomes stale when the source file is deleted and recreated (new inode)
        // On beegfs: target becomes inaccessible (stat fails)
        // On ext4/xfs: target is still accessible but inode/device mismatch with source
        bool is_mounted = ctx.graft->is_mounted(target);

        // Check for stale mount by comparing inode/device
        bool is_stale = false;
        struct stat source_st, target_st;
        bool source_stat_ok = (stat(source.c_str(), &source_st) == 0);
        bool target_stat_ok = (stat(target.c_str(), &target_st) == 0);

        if (is_mounted) {
            if (!target_stat_ok) {
                // Case 1: Target inaccessible (beegfs scenario)
                // Mount exists in /proc/mounts but stat() fails
                is_stale = true;
                utils::get_logger()->info("Stale mount detected (target inaccessible): {}", target.string());
            } else if (source_stat_ok) {
                // Case 2: Target accessible but inode/device mismatch (ext4/xfs scenario)
                // This happens when source file was deleted and recreated (new inode)
                if (source_st.st_ino != target_st.st_ino || source_st.st_dev != target_st.st_dev) {
                    is_stale = true;
                    utils::get_logger()->info("Stale mount detected (inode mismatch): source ino={} dev={}, target ino={} dev={}",
                        source_st.st_ino, source_st.st_dev, target_st.st_ino, target_st.st_dev);
                }
            }
        }

        if (is_stale) {
            // Unmount the stale mount and prepare for remount
            auto umount_result = utils::exec_safe({"umount", "-l", target.string()});
            if (umount_result.exit_code == 0) {
                utils::get_logger()->info("Lazy unmounted stale mount: {}", target.string());
                is_mounted = false;
                // Remove the empty mount point file if it exists
                std::error_code rm_ec;
                fs::remove(target, rm_ec);
                if (!rm_ec) {
                    utils::get_logger()->info("Removed empty mount point: {}", target.string());
                }
            } else {
                utils::get_logger()->warn("Failed to unmount stale mount {}: {}", target.string(), umount_result.stderr_output);
            }
        }

        if (is_mounted) {
            // Already mounted — still fix ownership of intermediate dirs and target
            // This handles the case where dirs were created by root on first run
            if (state.uid != 0 || state.gid != 0) {
                utils::get_logger()->info("Fixing ownership for already-mounted target: {}", target.string());
                fs::path boundary = home_dir.empty() ? fs::path(target.parent_path().parent_path()) : fs::path(home_dir);
                // Fix intermediate directories
                fs::path parent = target.parent_path();
                if (!parent.empty()) {
                    // chown intermediate dirs via exec_safe (chown_path_chain is static in graft.cpp)
                    fs::path p = parent;
                    std::vector<fs::path> to_fix;
                    while (!p.empty() && p != "/" && p != boundary) {
                        struct stat st;
                        if (stat(p.c_str(), &st) == 0 && (st.st_uid != state.uid || st.st_gid != state.gid)) {
                            to_fix.push_back(p);
                        }
                        p = p.parent_path();
                    }
                    for (auto it = to_fix.rbegin(); it != to_fix.rend(); ++it) {
                        auto r = utils::exec_safe({"chown",
                            std::to_string(state.uid) + ":" + std::to_string(state.gid),
                            it->string()});
                        if (r.exit_code == 0) {
                            utils::get_logger()->info("Fixed ownership: {} -> {}:{}", it->string(), state.uid, state.gid);
                        }
                    }
                }
                // Fix target itself
                struct stat tgt_st;
                if (stat(target.c_str(), &tgt_st) == 0 && (tgt_st.st_uid != state.uid || tgt_st.st_gid != state.gid)) {
                    auto r = utils::exec_safe({"chown",
                        std::to_string(state.uid) + ":" + std::to_string(state.gid),
                        target.string()});
                    if (r.exit_code == 0) {
                        utils::get_logger()->info("Fixed ownership: {} -> {}:{}", target.string(), state.uid, state.gid);
                    }
                }
            }
            continue;
        }

        utils::get_logger()->info("Fixing mount: {} -> {}", source.string(), target.string());
        if (ctx.graft->bind_mount(source, target, true, state.uid, state.gid, home_dir)) {
            fixes++;
        } else {
            mount_failures++;
            utils::get_logger()->warn("Failed to mount {} -> {}", source.string(), target.string());
        }
    }

    // Second pass: fix ownership of ALL existing mounts (including those not in current config)
    // This handles the case where mounts were created with an old config and
    // intermediate directories (e.g., .local/) were left as root:root
    if (state.uid != 0 || state.gid != 0) {
        auto all_mounts = ctx.graft->list_mounts(username);
        for (const auto& m : all_mounts) {
            fs::path boundary = fs::path(home_dir);
            fs::path parent = m.target.parent_path();
            if (!parent.empty()) {
                fs::path p = parent;
                std::vector<fs::path> to_fix;
                while (!p.empty() && p != "/" && p != boundary) {
                    struct stat st;
                    if (stat(p.c_str(), &st) == 0 && (st.st_uid != state.uid || st.st_gid != state.gid)) {
                        to_fix.push_back(p);
                    }
                    p = p.parent_path();
                }
                for (auto it = to_fix.rbegin(); it != to_fix.rend(); ++it) {
                    auto r = utils::exec_safe({"chown",
                        std::to_string(state.uid) + ":" + std::to_string(state.gid),
                        it->string()});
                    if (r.exit_code == 0) {
                        utils::get_logger()->info("Fixed ownership for existing mount parent: {} -> {}:{}", it->string(), state.uid, state.gid);
                        fixes++;
                    }
                }
            }
        }
    }

    // Clean up stale entries in home_dir that are not in current mount config.
    // Handles two cases:
    //   1. Broken symlinks (lstat ok but stat fails)
    //   2. Stale bind mounts whose source no longer exists (findmnt shows //deleted)
    // Both cause tools like lsd to print "No such file or directory" errors.
    {
        // Build set of currently configured mount targets under home_dir
        std::set<std::string> configured_mount_targets;
        for (const auto& mount_path : ctx.config.mount.paths) {
            auto source_opt = core::PathResolver::resolve(mount_path.string());
            if (!source_opt) continue;
            fs::path target = core::PathResolver::to_ai_user_path(*source_opt, username, main_user, home_dir);
            configured_mount_targets.insert(target.string());
        }

        // Scan home_dir top-level entries for stale mounts/broken symlinks
        std::error_code iter_ec;
        for (const auto& entry : fs::directory_iterator(home_dir, iter_ec)) {
            if (iter_ec) break;
            auto ep = entry.path();
            std::string ep_str = ep.string();

            // Skip entries that are in current mount config (they are valid)
            if (configured_mount_targets.count(ep_str)) continue;

            // Skip .am_status
            if (ep.filename() == ".am_status") continue;

            // Check if stat() fails but lstat() succeeds → broken symlink or stale mount
            struct stat st, lst;
            bool stat_ok = (stat(ep.c_str(), &st) == 0);
            bool lstat_ok = (lstat(ep.c_str(), &lst) == 0);

            if (!stat_ok && lstat_ok) {
                // Check if it's a stale bind mount (mount point with deleted source)
                bool is_stale_mount = ctx.graft->is_mounted_live(ep);

                if (is_stale_mount) {
                    // umount the stale bind mount
                    utils::get_logger()->info("Lazy unmounting stale bind mount: {}", ep_str);
                    auto umount_result = utils::exec_safe({"umount", "-l", ep_str});
                    if (umount_result.exit_code == 0) {
                        fixes++;
                        // Remove the empty mount point directory/file
                        std::error_code rm_ec;
                        fs::remove(ep, rm_ec);
                    } else {
                        utils::get_logger()->warn("Failed to umount stale mount {}: {}", ep_str, umount_result.stderr_output);
                    }
                } else if (S_ISLNK(lst.st_mode)) {
                    // Broken symlink — just remove it
                    utils::get_logger()->info("Removing broken symlink: {}", ep_str);
                    std::error_code rm_ec;
                    fs::remove(ep, rm_ec);
                    if (!rm_ec) {
                        fixes++;
                    } else {
                        utils::get_logger()->warn("Failed to remove broken symlink {}: {}", ep_str, rm_ec.message());
                    }
                }
            }
        }
    }

    // Third pass: recursively fix ownership of ALL entries in home_dir
    // Skip .am_status (root:root by design) and bind mount targets (read-only)
    {
        fs::path am_status_path = fs::path(home_dir) / ".am_status";

        // Get device number of home_dir for mount boundary detection
        struct stat home_st;
        dev_t home_dev = 0;
        if (stat(home_dir.c_str(), &home_st) == 0) {
            home_dev = home_st.st_dev;
        }

        // Recursive helper: chown all entries under a directory, skipping mount points
        // Returns number of fixes applied
        std::function<int(const fs::path&, int)> recursive_chown = [&](const fs::path& dir_path, int depth) -> int {
            constexpr int max_depth = 3;
            if (depth > max_depth) {
                utils::get_logger()->debug("Third pass: skipping depth {} > {} at {}", depth, max_depth, dir_path.string());
                return 0;
            }
            int local_fixes = 0;
            std::error_code iter_ec;
            for (const auto& entry : fs::directory_iterator(dir_path, iter_ec)) {
                if (iter_ec) break;
                auto ep = entry.path();

                // Skip .am_status
                if (ep == am_status_path) continue;

                struct stat st;
                if (lstat(ep.c_str(), &st) != 0) continue;

                // Skip symlinks — only fix symlink ownership, don't recurse
                if (S_ISLNK(st.st_mode)) {
                    if ((st.st_uid != state.uid || st.st_gid != state.gid)) {
                        if (lchown(ep.c_str(), state.uid, state.gid) == 0) {
                            utils::get_logger()->info("Third pass: fixed symlink {} -> {}:{}", ep.string(), state.uid, state.gid);
                            local_fixes++;
                        } else {
                            // Expected for read-only bind mounts or permission issues
                            utils::get_logger()->debug("Third pass: failed to fix symlink {}", ep.string());
                        }
                    }
                    continue;
                }

                // Check if this is a mount point by comparing device numbers
                // Bind mounts on same filesystem have same dev, so also check is_mounted()
                bool is_mount_point = false;
                if (S_ISDIR(st.st_mode)) {
                    // Device number changed = crossed a mount boundary
                    if (home_dev != 0 && st.st_dev != home_dev) {
                        is_mount_point = true;
                    }
                    // Also check via mount table (catches same-filesystem bind mounts)
                    if (!is_mount_point && ctx.graft->is_mounted_live(ep)) {
                        is_mount_point = true;
                    }
                } else if (S_ISREG(st.st_mode)) {
                    // Regular files can also be bind-mounted (e.g. dotfile mounts on beegfs)
                    if (ctx.graft->is_mounted_live(ep)) {
                        is_mount_point = true;
                    }
                }

                if (is_mount_point) {
                    // Don't chown or recurse into mount points (read-only bind mounts)
                    continue;
                }

                bool needs_chown = (st.st_uid != state.uid || st.st_gid != state.gid);
                if (needs_chown) {
                    int fd = open(ep.c_str(), O_RDONLY | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0) | O_NOFOLLOW);
                    if (fd >= 0) {
                        if (fchown(fd, state.uid, state.gid) == 0) {
                            utils::get_logger()->info("Third pass: fixed {} -> {}:{}", ep.string(), state.uid, state.gid);
                            local_fixes++;
                        } else {
                            // Expected for read-only bind mounts; these are caught by mount detection above
                            utils::get_logger()->debug("Third pass: failed to fix {}", ep.string());
                        }
                        close(fd);
                    } else if (errno == ELOOP) {
                        if (lchown(ep.c_str(), state.uid, state.gid) == 0) {
                            utils::get_logger()->info("Third pass: fixed symlink {} -> {}:{}", ep.string(), state.uid, state.gid);
                            local_fixes++;
                        }
                    }
                }

                // Recurse into sub-directories (non-mount, non-symlink)
                if (S_ISDIR(st.st_mode)) {
                    local_fixes += recursive_chown(ep, depth + 1);
                }
            }
            return local_fixes;
        };

        fixes += recursive_chown(fs::path(home_dir), 0);
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

    if (!utils::is_path_allowed(dir_path, main_user, ctx.config.user.allowed_bases)) {
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

    if (!utils::is_path_allowed_no_system_check(file_path, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Path not allowed: " << file_path.string() << std::endl;
        return 1;
    }

    if (!fs::exists(file_path)) {
        std::error_code ec;
        fs::path parent = file_path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            if (!utils::is_path_allowed_no_system_check(parent, main_user, ctx.config.user.allowed_bases)) {
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

    if (!utils::is_path_allowed(dst_path, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Destination path not allowed: " << dst_path.string() << std::endl;
        return 1;
    }

    if (!utils::is_path_allowed(src_path, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Source path not allowed: " << src_path.string() << std::endl;
        return 1;
    }

    // --- Dual ai-user detection ---
    // Detect ai-user from source path (file itself and its parent directory)
    std::string src_ai_user = core::PathResolver::detect_ai_user_from_path(src_path, main_user, ctx.config.user.prefix);
    if (src_ai_user.empty()) {
        src_ai_user = core::PathResolver::detect_ai_user_from_path(src_path.parent_path(), main_user, ctx.config.user.prefix);
    }
    if (src_ai_user.empty()) {
        std::string src_owner = core::PathResolver::detect_owner_user(src_path);
        if (!src_owner.empty() && validate_ai_user_ownership(src_owner, main_user, ctx.config.user.prefix)) {
            src_ai_user = src_owner;
        }
    }

    // Detect ai-user from destination path (path components first, then stat-based)
    std::string dst_ai_user = core::PathResolver::detect_ai_user_from_path(dst_path, main_user, ctx.config.user.prefix);
    if (dst_ai_user.empty() && fs::is_directory(dst_path)) {
        std::string dir_owner = core::PathResolver::detect_owner_user(dst_path);
        if (!dir_owner.empty() && validate_ai_user_ownership(dir_owner, main_user, ctx.config.user.prefix)) {
            dst_ai_user = dir_owner;
        }
    }

    // --- Source ai-user security check ---
    if (!src_ai_user.empty()) {
        if (!validate_ai_user_ownership(src_ai_user, main_user, ctx.config.user.prefix)) {
            std::cerr << "Source belongs to ai-user '" << src_ai_user
                      << "' which does not belong to user '" << main_user << "'" << std::endl;
            return 1;
        }
    }

    // --- Destination ai-user security check ---
    if (!dst_ai_user.empty()) {
        if (!validate_ai_user_ownership(dst_ai_user, main_user, ctx.config.user.prefix)) {
            std::cerr << "Destination ai-user '" << dst_ai_user
                      << "' does not belong to user '" << main_user << "'" << std::endl;
            return 1;
        }
    }

    // --- Determine chown target ---
    // Same logic as cmd_mv: A=dst_ai, B=dst_ai, C=main_user, D=rejected, E=none
    bool need_chown = false;
    std::string chown_user;

    if (!dst_ai_user.empty()) {
        need_chown = true;
        chown_user = dst_ai_user;
    } else if (!src_ai_user.empty()) {
        need_chown = true;
        chown_user = main_user;
    }

    mode_t old_umask = umask(0077);
    auto cp_result = utils::exec_safe({"cp", "-rP", "--no-preserve=mode", src_path.string(), dst_path.string()});
    if (cp_result.exit_code != 0) {
        umask(old_umask);
        std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
        return 1;
    }

    if (need_chown) {
        fs::path chown_target = fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
        if (!safe_chown_path(chown_target, chown_user)) {
            umask(old_umask);
            std::cerr << "Failed to set ownership for " << chown_user << std::endl;
            return 1;
        }
    }
    umask(old_umask);

    if (verbose) {
        if (need_chown) {
            std::cout << "Copied: " << src_path.string() << " -> " << dst_path.string() << " (owner: " << chown_user << ")" << std::endl;
        } else {
            std::cout << "Copied: " << src_path.string() << " -> " << dst_path.string() << std::endl;
        }
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

    if (!utils::is_path_allowed(dst_path, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Destination path not allowed: " << dst_path.string() << std::endl;
        return 1;
    }

    if (!utils::is_path_allowed(src_path, main_user, ctx.config.user.allowed_bases)) {
        std::cerr << "Source path not allowed: " << src_path.string() << std::endl;
        return 1;
    }

    // --- Dual ai-user detection ---
    // Detect ai-user from source path (file itself and its parent directory)
    std::string src_ai_user = core::PathResolver::detect_ai_user_from_path(src_path, main_user, ctx.config.user.prefix);
    if (src_ai_user.empty()) {
        src_ai_user = core::PathResolver::detect_ai_user_from_path(src_path.parent_path(), main_user, ctx.config.user.prefix);
    }
    // If still empty, try stat-based owner detection
    if (src_ai_user.empty()) {
        std::string src_owner = core::PathResolver::detect_owner_user(src_path);
        if (!src_owner.empty() && validate_ai_user_ownership(src_owner, main_user, ctx.config.user.prefix)) {
            src_ai_user = src_owner;
        }
    }

    // Detect ai-user from destination path (path components first)
    std::string dst_ai_user = core::PathResolver::detect_ai_user_from_path(dst_path, main_user, ctx.config.user.prefix);
    // If dst is a directory, the file will be placed inside it — check directory owner via stat
    if (dst_ai_user.empty() && fs::is_directory(dst_path)) {
        std::string dir_owner = core::PathResolver::detect_owner_user(dst_path);
        if (!dir_owner.empty() && validate_ai_user_ownership(dir_owner, main_user, ctx.config.user.prefix)) {
            dst_ai_user = dir_owner;
        }
    }

    // --- Source ai-user security check ---
    // If src belongs to an ai-user, verify it belongs to the current main-user
    if (!src_ai_user.empty()) {
        if (!validate_ai_user_ownership(src_ai_user, main_user, ctx.config.user.prefix)) {
            std::cerr << "Source belongs to ai-user '" << src_ai_user
                      << "' which does not belong to user '" << main_user << "'" << std::endl;
            return 1;
        }
    }

    // --- Destination ai-user security check ---
    if (!dst_ai_user.empty()) {
        if (!validate_ai_user_ownership(dst_ai_user, main_user, ctx.config.user.prefix)) {
            std::cerr << "Destination ai-user '" << dst_ai_user
                      << "' does not belong to user '" << main_user << "'" << std::endl;
            return 1;
        }
    }

    // --- Cross main-user leak prevention (Scenario D) ---
    // If src is ai-user and dst is a different ai-user, ensure both belong to same main-user
    if (!src_ai_user.empty() && !dst_ai_user.empty() && src_ai_user != dst_ai_user) {
        // Both have already passed validate_ai_user_ownership against main_user,
        // so they belong to the same main-user. This is safe (Scenario B).
    }

    // --- Determine chown target ---
    // Scenario A: main→ai → chown to dst_ai_user
    // Scenario B: ai→ai (same main) → chown to dst_ai_user
    // Scenario C: ai→main → chown back to main_user
    // Scenario D: cross-main → already rejected above
    // Scenario E: main→main → no chown needed
    bool need_chown = false;
    std::string chown_user;

    if (!dst_ai_user.empty()) {
        // Scenarios A/B: destination is ai-user territory
        need_chown = true;
        chown_user = dst_ai_user;
    } else if (!src_ai_user.empty()) {
        // Scenario C: moving from ai-user to main-user territory
        need_chown = true;
        chown_user = main_user;
    }
    // else: Scenario E, no chown needed

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
            if (!safe_chown_path(chown_target, chown_user)) {
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
                std::cout << "Moved (copy+delete): " << src_path.string() << " -> " << dst_path.string() << " (owner: " << chown_user << ")" << std::endl;
            } else {
                std::cout << "Moved (copy+delete): " << src_path.string() << " -> " << dst_path.string() << std::endl;
            }
        }
        return 0;
    }

    if (need_chown) {
        fs::path chown_target = fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
        if (!safe_chown_path(chown_target, chown_user)) {
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
            std::cout << "Moved (atomic): " << src_path.string() << " -> " << dst_path.string() << " (owner: " << chown_user << ")" << std::endl;
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

        // Check if key is in authorized_keys (needed for both debug and health check)
        fs::path ssh_dir = fs::path(state->home_dir) / ".ssh";
        fs::path auth_keys = ssh_dir / "authorized_keys";
        bool key_authorized = false;
        if (fs::exists(auth_keys, ec) && !ec) {
            std::ifstream ak(auth_keys);
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

        // Debug line for SSH troubleshooting (only when issues detected)
        if (has_broken || missing_ssh || !key_authorized) {
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
    std::cout << "User prefix: " << config.user.prefix << " (default)" << std::endl;

    if (!config.user.allowed_bases.empty()) {
        std::cout << "Allowed bases:" << std::endl;
        for (const auto& b : config.user.allowed_bases) {
            std::cout << "  - " << b.string() << std::endl;
        }
    }

    std::cout << "SSH key type: " << config.ssh.key_type << std::endl;
    std::cout << "SSH key path: " << config.ssh.key_path.string() << std::endl;
    std::cout << "SSH default key: " << config.ssh.ai_default_key.string() << std::endl;

    std::cout << "Mount paths:" << std::endl;
    for (const auto& p : config.mount.paths) {
        std::cout << "  - " << p.string() << std::endl;
    }

    if (!config.ai_user.groups.empty()) {
        std::cout << "AI-user groups:" << std::endl;
        for (const auto& g : config.ai_user.groups) {
            std::cout << "  - " << g << std::endl;
        }
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

// ============================================================================
// cmd_watch: FTXUI-based real-time monitoring for ai-users
// ============================================================================

namespace {
    using namespace ftxui;

    // Format memory with auto unit selection
    std::string format_memory(unsigned long mb) {
        unsigned long kb = mb * 1024;
        std::ostringstream ss;
        if (kb < 1024) {
            ss << kb << "K";
        } else if (kb < 1024 * 1024) {
            ss << std::fixed << std::setprecision(1) << (kb / 1024.0) << "M";
        } else {
            ss << std::fixed << std::setprecision(2) << (kb / (1024.0 * 1024.0)) << "G";
        }
        return ss.str();
    }

    // Pad string to fixed width (truncate if too long)
    std::string pad_col(const std::string& s, size_t width) {
        if (s.size() >= width) return s.substr(0, width);
        return s + std::string(width - s.size(), ' ');
    }

    // Build FTXUI table from stats (sorted: active users first)
    Element render_stats_table(std::vector<daemon::UserStats> stats) {
        // Sort: logged_in users first, then by CPU%
        std::stable_sort(stats.begin(), stats.end(),
            [](const daemon::UserStats& a, const daemon::UserStats& b) {
                if (a.logged_in != b.logged_in) return a.logged_in > b.logged_in;
                return a.cpu_percent > b.cpu_percent;
            });

        // Column widths: Username=24, CPU%=8, Mem=10, Procs=6, SSH=8
        auto header = hbox({
            text(pad_col("Username", 24)) | bold,
            text(pad_col("CPU%", 8)) | bold,
            text(pad_col("Mem", 10)) | bold,
            text(pad_col("Procs", 6)) | bold,
            text(pad_col("SSH", 8)) | bold,
        });

        std::vector<Element> rows;
        rows.push_back(header);
        rows.push_back(text(std::string(56, '-')) | dim);  // underline below header

        size_t row_idx = 0;
        for (const auto& s : stats) {
            std::ostringstream cpu_ss;
            cpu_ss << std::fixed << std::setprecision(1) << s.cpu_percent;

            // CPU color: gradient from gray → green → yellow → red
            Color cpu_color;
            if (s.cpu_percent > 80)      cpu_color = Color::Red;
            else if (s.cpu_percent > 60) cpu_color = Color::OrangeRed1;
            else if (s.cpu_percent > 40) cpu_color = Color::Yellow;
            else if (s.cpu_percent > 20) cpu_color = Color::Green;
            else if (s.cpu_percent > 5)  cpu_color = Color::GreenLight;
            else                         cpu_color = Color::GrayLight;

            // Username: bold if active
            auto name_elem = s.logged_in
                ? text(pad_col(s.username, 24)) | bold
                : text(pad_col(s.username, 24));

            // SSH: light green + bold if active
            Element ssh_elem = s.logged_in
                ? text(pad_col("active", 8)) | color(Color::GreenLight) | bold
                : text(pad_col("no", 8)) | dim;

            rows.push_back(hbox({
                name_elem,
                text(pad_col(cpu_ss.str(), 8)) | color(cpu_color),
                text(pad_col(format_memory(s.memory_mb), 10)),
                text(pad_col(std::to_string(s.process_count), 6)),
                std::move(ssh_elem),
            }));
            row_idx++;

            // Separator every 5 rows for visual grouping
            if (row_idx % 5 == 0 && row_idx < stats.size()) {
                rows.push_back(text(std::string(56, '-')) | dim);
            }
        }

        return vbox(rows) | border;
    }
} // anonymous namespace

int cmd_watch(const std::string& watch_path, const std::string& watch_user, bool verbose) {
    auto ctx = make_context(verbose);
    std::string main_user = utils::get_effective_username();

    // Resolve optional path filter
    std::optional<std::string> path_filter;
    if (!watch_path.empty()) {
        auto resolved = core::PathResolver::resolve(watch_path);
        if (!resolved) {
            std::cerr << "Invalid path: " << watch_path << std::endl;
            return 1;
        }
        path_filter = resolved->string();
    }

    // Print startup message (for test compatibility)
    std::cout << "Starting ai-mirror watch... Press Ctrl+C to exit.\n";

    auto screen = ScreenInteractive::Fullscreen();

    // Shared state for periodic data refresh
    auto stats = std::make_shared<std::vector<daemon::UserStats>>();
    auto empty_msg = std::make_shared<bool>(true);
    auto refresh_interval = std::make_shared<int>(5);  // Default 5 seconds

    // Cached user list — only refresh every 30s to avoid re-reading /etc/passwd
    constexpr auto user_list_ttl = std::chrono::seconds(30);
    auto cached_usernames = std::make_shared<std::vector<std::string>>();
    auto cached_uids = std::make_shared<std::vector<uid_t>>();
    auto last_user_refresh = std::make_shared<std::chrono::steady_clock::time_point>();

    // Periodic data refresh via a separate thread
    std::atomic<bool> running{true};
    std::thread refresh_thread([&]() {
        while (running) {
            auto now = std::chrono::steady_clock::now();

            // Refresh user list only every 30 seconds
            bool need_user_refresh = !last_user_refresh
                || (now - *last_user_refresh) >= user_list_ttl;

            if (need_user_refresh) {
                auto users = ctx.user_mgr->list_ai_users();
                std::string expected_prefix = ctx.config.user.prefix + main_user + "_";
                std::vector<std::string> usernames;
                std::vector<uid_t> uids;

                for (const auto& u : users) {
                    if (u.username.size() > expected_prefix.size()
                        && u.username.substr(0, expected_prefix.size()) == expected_prefix) {
                        
                        // Filter by user name if provided
                        if (!watch_user.empty() && u.username != watch_user) {
                            continue;
                        }

                        // Filter by project path if provided
                        if (path_filter) {
                            // User's home should match project path
                            if (u.home_dir != *path_filter) {
                                continue;
                            }
                        }

                        usernames.push_back(u.username);
                        uids.push_back(u.uid);
                    }
                }

                *cached_usernames = std::move(usernames);
                *cached_uids = std::move(uids);
                *last_user_refresh = now;
            }

            if (cached_usernames->empty()) {
                *empty_msg = true;
                stats->clear();
            } else {
                *empty_msg = false;
                // Single /proc scan + single ps call for all users
                auto uid_stats = daemon::gather_all_uid_stats();
                *stats = daemon::build_user_stats(*cached_usernames, *cached_uids, uid_stats);
            }

            screen.PostEvent(Event::Custom);

            // Sleep with interrupt check (100ms granularity)
            for (int i = 0; i < *refresh_interval * 10 && running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    auto renderer = Renderer([&, stats, empty_msg, main_user, refresh_interval] {
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        std::ostringstream time_ss;
        time_ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");

        if (*empty_msg) {
            return vbox({
                text("No ai-users found for " + main_user) | color(Color::Yellow) | bold | hcenter,
                text("Create one with: am create <project_path>") | dim | hcenter,
            }) | border | flex;
        }

        return vbox({
            hbox({
                text("ai-mirror watch - " + main_user + "'s ai-users") | bold | color(Color::Cyan),
                filler(),
                text("Press q/Esc to exit") | dim,
            }),
            separator(),
            render_stats_table(*stats),
            separator(),
            hbox({
                text("Refresh: " + std::to_string(*refresh_interval) + "s") | color(Color::Cyan),
                text(" | "),
                text(std::to_string(stats->size()) + " users") | color(Color::Cyan),
                text(" | "),
                text("Last update: " + time_ss.str()) | color(Color::Cyan),
            }),
        }) | border | flex;
    });

    auto component = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Character('q') || event == Event::Escape) {
            screen.Exit();
            return true;
        }
        // FTXUI handles Ctrl+C automatically via SIGINT
        return false;
    });

    screen.Loop(component);

    running = false;
    refresh_thread.join();

    // Print exit message (for test compatibility)
    std::cout << "Watch stopped.\n";
    std::cout.flush();

    return 0;
}

// cmd_init: Initialize user environment for am command
// Ensures am command works in all shell contexts (login, non-login, tmux, screen)
int cmd_init([[maybe_unused]] bool verbose) {
    std::string user = utils::get_effective_username();
    std::string home = utils::get_home_dir(user);
    std::string bashrc = home + "/.bashrc";
    std::string config_file = home + "/.ai-mirror.toml";

    std::cout << "=== ai-mirror 初始化检查 ===\n\n";

    // 1. Check ai-mirror group membership
    bool in_group = utils::is_group_member("ai-mirror");
    std::cout << "1. ai-mirror 组成员身份: ";
    if (in_group) {
        std::cout << "✓ 已在组中\n";
    } else {
        std::cout << "✗ 未在组中\n";
        std::cout << "   修复命令: sudo usermod -aG ai-mirror " << user << "\n";
        std::cout << "   修复后需重新登录生效\n";
    }

    // 2. Check config file exists
    std::cout << "\n2. 用户配置文件 ~/.ai-mirror.toml: ";
    bool config_exists = fs::exists(config_file);
    if (config_exists) {
        std::cout << "✓ 已存在\n";
    } else {
        std::cout << "○ 不存在（将使用默认配置）\n";
        // Create minimal config
        std::ofstream ofs(config_file);
        if (ofs.is_open()) {
            ofs << "# ai-mirror 用户配置\n";
            ofs << "# 首次运行自动创建，覆盖默认值\n\n";
            ofs << "[mount]\n";
            ofs << "paths = [\n";
            ofs << "    \"~/.bashrc\",\n";
            ofs << "    \"~/.config\",\n";
            ofs << "    \"~/.local/bin\",\n";
            ofs << "]\n\n";
            ofs << "[ssh]\n";
            ofs << "key_type = \"ed25519\"\n";
            ofs.close();
            std::cout << "   → 已创建默认配置文件\n";
        } else {
            std::cout << "   → 创建失败（权限问题）\n";
        }
    }

    // 3. Check if ~/.bashrc sources am.sh (critical for tmux)
    std::cout << "\n3. ~/.bashrc 中 am.sh 加载: ";
    bool bashrc_has_source = false;
    std::ifstream bashrc_in(bashrc);
    if (bashrc_in.is_open()) {
        std::string line;
        while (std::getline(bashrc_in, line)) {
            // Check for various forms of sourcing am.sh
            if (line.find("source /etc/profile.d/am.sh") != std::string::npos ||
                line.find(". /etc/profile.d/am.sh") != std::string::npos ||
                line.find("source ~/.local/share/bash-completion/completions/am") != std::string::npos) {
                bashrc_has_source = true;
                break;
            }
        }
        bashrc_in.close();
    }

    if (bashrc_has_source) {
        std::cout << "✓ 已配置\n";
    } else {
        std::cout << "○ 未配置（tmux/screen 等非登录 shell 可能无法使用 am）\n";

        // Append source line to ~/.bashrc
        std::ofstream bashrc_out(bashrc, std::ios::app);
        if (bashrc_out.is_open()) {
            bashrc_out << "\n# ai-mirror: 确保 am 命令在 tmux/screen 等非登录 shell 中可用\n";
            bashrc_out << "if [[ -f /etc/profile.d/am.sh ]]; then\n";
            bashrc_out << "    source /etc/profile.d/am.sh\n";
            bashrc_out << "fi\n";
            bashrc_out.close();
            std::cout << "   → 已追加配置到 ~/.bashrc\n";
        } else {
            std::cout << "   → 写入失败（请手动添加）\n";
        }
    }

    // 4. Check bash completion
    std::cout << "\n4. Bash 补齐: ";
    bool completion_installed = fs::exists("/etc/bash_completion.d/am");
    if (completion_installed) {
        std::cout << "✓ 已安装 (/etc/bash_completion.d/am)\n";
    } else {
        std::cout << "○ 未安装系统级补齐\n";
        std::cout << "   修复命令: sudo cp completions/am-completion.bash /etc/bash_completion.d/am\n";
    }

    // 5. Summary
    std::cout << "\n=== 总结 ===\n";
    if (in_group && bashrc_has_source) {
        std::cout << "环境已完整配置。\n";
        std::cout << "新终端/tmux 窗口可直接使用 am 命令。\n";
    } else {
        std::cout << "部分配置缺失，请按上述提示修复。\n";
        if (!in_group) {
            std::cout << "关键: 请先加入 ai-mirror 组并重新登录。\n";
        }
        if (!bashrc_has_source) {
            std::cout << "提示: ~/.bashrc 已更新，新终端自动生效。\n";
        }
    }

    std::cout << "\n如需立即生效，执行: source ~/.bashrc\n";

    return (in_group && bashrc_has_source) ? 0 : 1;
}

}

#include "ai_mirror/cli/commands.hpp"
#include "ai_mirror/core/user_manager.hpp"
#include "ai_mirror/core/graft.hpp"
#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/core/config.hpp"
#include "ai_mirror/core/path_resolver.hpp"
#include "ai_mirror/daemon/health_check.hpp"
#include "ai_mirror/daemon/mount_cleaner.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

namespace fs = std::filesystem;

namespace ai_mirror::cli {

static bool validate_ai_user_ownership(const std::string& ai_user, const std::string& main_user, const std::string& prefix) {
    if (ai_user.empty() || main_user.empty()) return false;
    std::string expected_prefix = prefix + main_user;
    return ai_user.size() >= expected_prefix.size()
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
    ctx.user_mgr = std::make_unique<core::UserManager>(ctx.config.user.prefix);
    ctx.graft = std::make_unique<core::Graft>(ctx.config.user.prefix);
    ctx.ssh_mgr = std::make_unique<core::SSHManager>();
    ctx.verbose = verbose;
    return ctx;
}

int cmd_create(const std::string& project_path, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror create requires root privileges" << std::endl;
        return 1;
    }

    fs::path proj = core::PathResolver::resolve(project_path);
    if (proj.empty()) {
        std::cerr << "Invalid project path: " << project_path << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();
    if (!utils::is_path_allowed(proj, main_user)) {
        std::cerr << "Path not allowed: " << proj.string() << std::endl;
        return 1;
    }

    utils::get_logger()->info("Creating ai-user for project: {}", proj.string());

    auto user_info = ctx.user_mgr->create_ai_user(proj.string());
    if (!user_info.exists) {
        std::cerr << "Failed to create ai-user for " << proj.string() << std::endl;
        return 1;
    }

    ctx.ssh_mgr->set_key_path(ctx.config.ssh.key_path);
    ctx.ssh_mgr->set_key_type(ctx.config.ssh.key_type);

    if (!ctx.ssh_mgr->setup_passwordless(main_user, user_info.username)) {
        std::cerr << "Warning: SSH setup failed" << std::endl;
    }

    if (!ctx.config.ssh.ai_default_key.empty()) {
        ctx.ssh_mgr->setup_default_key_from_file(user_info.username, ctx.config.ssh.ai_default_key);
    }

    for (const auto& mount_path : ctx.config.mount.paths) {
        fs::path source = core::PathResolver::resolve(mount_path.string());
        if (!fs::exists(source)) {
            utils::get_logger()->warn("Mount source does not exist, skipping: {}", source.string());
            continue;
        }

        if (!utils::is_path_allowed(source, main_user)) {
            utils::get_logger()->error("Mount source path not allowed, skipping: {}", source.string());
            continue;
        }

        fs::path target = core::PathResolver::to_ai_user_path(source, user_info.username, main_user);
        ctx.graft->bind_mount(source, target, true);
    }

    ctx.graft->grant_write_access(proj, user_info.username);

    std::cout << user_info.username << std::endl;
    return 0;
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

static bool safe_chown_path(const fs::path& p, const std::string& owner) {
    struct passwd* pw = getpwnam(owner.c_str());
    if (!pw) {
        utils::get_logger()->error("safe_chown_path: user '{}' not found", owner);
        return false;
    }
    if (!fs::is_directory(p)) {
        return safe_chown_single(p, pw->pw_uid, pw->pw_gid);
    }

    std::vector<fs::path> entries;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            utils::get_logger()->warn("safe_chown_path: iterator error: {}", ec.message());
            ec.clear();
            continue;
        }
        if (fs::is_symlink(it->symlink_status(ec))) {
            continue;
        }
        entries.push_back(it->path());
    }

    for (auto rit = entries.rbegin(); rit != entries.rend(); ++rit) {
        if (!safe_chown_single(*rit, pw->pw_uid, pw->pw_gid)) {
            return false;
        }
    }

    if (!safe_chown_single(p, pw->pw_uid, pw->pw_gid)) {
        return false;
    }

    auto chmod_result = utils::exec_safe({"chmod", "-R", "ug-s", p.string()});
    if (chmod_result.exit_code != 0) {
        utils::get_logger()->warn("safe_chown_path: chmod ug-s failed for {}: {}", p.string(), chmod_result.stderr_output);
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

    fs::path dir_path = core::PathResolver::resolve(path);
    if (dir_path.empty()) {
        std::cerr << "Invalid path: " << path << std::endl;
        return 1;
    }

    if (!utils::is_path_allowed(dir_path, main_user)) {
        std::cerr << "Path not allowed: " << dir_path.string() << std::endl;
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(dir_path, ec)) {
        fs::create_directories(dir_path, ec);
        if (ec) {
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

    fs::path file_path = core::PathResolver::resolve(path);
    if (file_path.empty()) {
        std::cerr << "Invalid path: " << path << std::endl;
        return 1;
    }

    if (!utils::is_path_allowed(file_path, main_user)) {
        std::cerr << "Path not allowed: " << file_path.string() << std::endl;
        return 1;
    }

    if (!fs::exists(file_path)) {
        std::error_code ec;
        fs::path parent = file_path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            fs::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Failed to create parent directory: " << parent.string() << std::endl;
                return 1;
            }
        }
        std::ofstream ofs(file_path);
        if (!ofs.is_open()) {
            std::cerr << "Failed to create file: " << file_path.string() << std::endl;
            return 1;
        }
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

    fs::path src_path = core::PathResolver::resolve(src);
    if (src_path.empty() || !fs::exists(src_path)) {
        std::cerr << "Source does not exist: " << src << std::endl;
        return 1;
    }

    fs::path dst_path = core::PathResolver::resolve(dst);
    if (dst_path.empty()) {
        std::cerr << "Invalid destination path: " << dst << std::endl;
        return 1;
    }

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
        auto cp_result = utils::exec_safe({"cp", "-r", "--no-preserve=mode", src_path.string(), dst_path.string()});
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

    auto cp_result = utils::exec_safe({"cp", "-r", "--no-preserve=mode", src_path.string(), dst_path.string()});
    if (cp_result.exit_code != 0) {
        std::cerr << "Copy failed: " << cp_result.stderr_output << std::endl;
        return 1;
    }

    fs::path chown_target = fs::is_directory(dst_path) ? dst_path / src_path.filename() : dst_path;
    if (!safe_chown_path(chown_target, ai_user)) {
        std::cerr << "Failed to set ownership for " << ai_user << std::endl;
        return 1;
    }

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

    fs::path src_path = core::PathResolver::resolve(src);
    if (src_path.empty() || !fs::exists(src_path)) {
        std::cerr << "Source does not exist: " << src << std::endl;
        return 1;
    }

    fs::path dst_path = core::PathResolver::resolve(dst);
    if (dst_path.empty()) {
        std::cerr << "Invalid destination path: " << dst << std::endl;
        return 1;
    }

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
        auto cp_result = utils::exec_safe({"cp", "-r", "--no-preserve=mode", src_path.string(), dst_path.string()});
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

        fs::remove_all(src_path, ec);
        if (ec) {
            utils::get_logger()->warn("Failed to remove source after copy: {}", ec.message());
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
            utils::get_logger()->warn("Rename succeeded but chown failed for {}", chown_target.string());
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

    fs::path target = core::PathResolver::resolve(path);
    if (!fs::exists(target)) {
        std::cerr << "Path does not exist: " << path << std::endl;
        return 1;
    }

    std::string target_str = target.string();
    if (!utils::validate_path_no_shell_metachars(target_str)) {
        std::cerr << "Path contains disallowed characters" << std::endl;
        return 1;
    }

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

    std::cout << "ai-mirror managed users:" << std::endl;
    for (const auto& u : users) {
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
    daemon::HealthCheck hc;
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
    if (!ctx.user_mgr->user_exists(username)) {
        auto derived = ctx.user_mgr->derive_username(project_or_user);
        if (!derived) {
            std::cerr << "Username collision: cannot derive unique username for: " << project_or_user << std::endl;
            return 1;
        }
        username = std::move(*derived);
        if (!ctx.user_mgr->user_exists(username)) {
            std::cerr << "User not found: " << project_or_user << std::endl;
            return 1;
        }
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

int cmd_rm(const std::string& project_path, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror rm requires root privileges" << std::endl;
        return 1;
    }

    fs::path proj = core::PathResolver::resolve(project_path);
    if (proj.empty()) {
        std::cerr << "Invalid project path: " << project_path << std::endl;
        return 1;
    }

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
    ctx.graft->revoke_write_access(proj, username);

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

    for (const auto& u : users) {
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

        std::string ssh_key_path = utils::get_home_dir(main_user) + "/.ssh/ai-mirror";
        bool ssh_ok = fs::exists(fs::path(ssh_key_path)) &&
                      fs::exists(fs::path(ssh_key_path + ".pub"));
        std::cout << "  SSH:   " << (ssh_ok ? "ok" : "missing") << std::endl;

        fs::path auth_keys = fs::path(u.home_dir) / ".ssh" / "authorized_keys";
        std::cout << "  Auth:  " << (fs::exists(auth_keys) ? "ok" : "missing") << std::endl;

        std::cout << "  Status: " << (all_healthy ? "healthy" : "unhealthy") << std::endl;
        std::cout << std::endl;
    }

    return 0;
}

}

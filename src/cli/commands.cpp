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
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace ai_mirror::cli {

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
    ctx.graft = std::make_unique<core::Graft>();
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

    utils::get_logger()->info("Creating ai-user for project: {}", proj.string());

    auto user_info = ctx.user_mgr->create_ai_user(proj.string());
    if (!user_info.exists) {
        std::cerr << "Failed to create ai-user for " << proj.string() << std::endl;
        return 1;
    }

    std::string main_user = utils::get_effective_username();
    ctx.ssh_mgr->set_key_path(ctx.config.ssh.key_path);
    ctx.ssh_mgr->set_key_type(ctx.config.ssh.key_type);

    if (!ctx.ssh_mgr->setup_passwordless(main_user, user_info.username)) {
        std::cerr << "Warning: SSH setup failed" << std::endl;
    }

    if (!ctx.config.ssh.default_keys.empty()) {
        ctx.ssh_mgr->setup_default_keys(user_info.username, ctx.config.ssh.default_keys);
    }

    for (const auto& mount_path : ctx.config.mount.paths) {
        fs::path source = core::PathResolver::resolve(mount_path.string());
        if (!fs::exists(source)) {
            utils::get_logger()->warn("Mount source does not exist, skipping: {}", source.string());
            continue;
        }

        fs::path target = core::PathResolver::to_ai_user_path(source, user_info.username, main_user);
        ctx.graft->bind_mount(source, target, true);
    }

    ctx.graft->grant_write_access(proj, user_info.username);

    std::cout << user_info.username << std::endl;
    return 0;
}

int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose) {
    auto ctx = make_context(verbose);

    if (!utils::is_root()) {
        std::cerr << "ai-mirror mkdir requires root privileges" << std::endl;
        return 1;
    }

    fs::path dir_path = core::PathResolver::resolve(path);
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

int cmd_cd(const std::string& path, bool verbose) {
    fs::path target = core::PathResolver::resolve(path);
    if (!fs::exists(target)) {
        std::cerr << "Path does not exist: " << path << std::endl;
        return 1;
    }

    std::string owner = core::PathResolver::detect_owner_user(target);
    std::string current_user = utils::get_effective_username();

    if (owner.empty() || owner == current_user) {
        std::cout << "cd " << target.string() << std::endl;
        return 0;
    }

    std::string prefix = "i";
    if (owner.substr(0, prefix.length()) == prefix) {
        if (verbose) {
            std::cerr << "Switching to user: " << owner << std::endl;
        }
        std::cout << "ssh " << owner << "@localhost" << std::endl;
        return 0;
    }

    std::cout << "cd " << target.string() << std::endl;
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
        fs::path proj(project_or_user);
        username = ctx.user_mgr->derive_username(project_or_user);
        if (!ctx.user_mgr->user_exists(username)) {
            std::cerr << "User not found: " << project_or_user << std::endl;
            return 1;
        }
    }

    utils::get_logger()->warn("Force destroying user: {}", username);

    daemon::MountCleaner cleaner;
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

    std::string username = ctx.user_mgr->derive_username(proj.string());
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
    std::string main_user = utils::get_effective_username();
    fs::path main_home = utils::get_effective_home();
    fs::path preserved_dir = main_home / ".ai-mirror-preserves" / username;

    utils::get_logger()->info("Removing project: {} (user: {})", proj.string(), username);

    if (verbose) {
        std::cout << "Step 1: Finding output files outside ai-user home" << std::endl;
    }

    std::ostringstream find_cmd;
    find_cmd << "find / -user " << username
             << " -not -path '" << ai_home.string() << "/*'"
             << " -not -path '" << ai_home.string() << "'"
             << " -type f 2>/dev/null";
    auto find_result = utils::execute(find_cmd.str());

    bool has_output = false;
    if (!find_result.stdout_output.empty()) {
        std::istringstream stream(find_result.stdout_output);
        std::string file_path;
        while (std::getline(stream, file_path)) {
            if (file_path.empty()) continue;
            if (!has_output) {
                std::error_code ec;
                fs::create_directories(preserved_dir, ec);
                if (ec) {
                    std::cerr << "Failed to create preserve dir: " << preserved_dir.string() << std::endl;
                    return 1;
                }
                has_output = true;
            }

            fs::path src(file_path);
            fs::path dst = preserved_dir / src.relative_path();
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                utils::get_logger()->warn("Failed to preserve file: {} - {}", file_path, ec.message());
            } else if (verbose) {
                std::cout << "  Preserved: " << file_path << std::endl;
            }
        }
    }

    if (has_output) {
        std::ostringstream chown_cmd;
        chown_cmd << "chown -R " << main_user << ":" << main_user << " " << preserved_dir.string();
        utils::execute(chown_cmd.str());
        if (verbose) {
            std::cout << "  Output files preserved to " << preserved_dir.string() << std::endl;
        }
    } else if (verbose) {
        std::cout << "  No output files found outside ai-user home." << std::endl;
    }

    if (verbose) {
        std::cout << "Step 2: Unmounting bind mounts for " << username << std::endl;
    }
    daemon::MountCleaner cleaner;
    cleaner.cleanup_for_user(username);

    if (verbose) {
        std::cout << "Step 3: Removing user " << username << std::endl;
    }
    if (!ctx.user_mgr->remove_ai_user(username, false)) {
        std::cerr << "Failed to remove user: " << username << std::endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Step 4: Cleaning up ai-user home" << std::endl;
    }
    {
        std::error_code ec;
        fs::remove_all(ai_home, ec);
        if (ec) {
            utils::get_logger()->warn("Failed to clean home dir: {}", ec.message());
        }
    }

    if (verbose) {
        std::cout << "Step 5: Revoking write grants on project" << std::endl;
    }
    ctx.graft->revoke_write_access(proj, username);

    std::cout << "Removed: " << username << std::endl;
    if (has_output) {
        std::cout << "Output preserved at: " << preserved_dir.string() << std::endl;
    }
    return 0;
}

int cmd_config([[maybe_unused]] bool verbose) {
    auto config = core::ConfigParser::load_default();
    std::cout << "Config file: " << config.config_path.string() << std::endl;
    std::cout << "User prefix: " << config.user.prefix << std::endl;
    std::cout << "SSH key type: " << config.ssh.key_type << std::endl;
    std::cout << "SSH key path: " << config.ssh.key_path.string() << std::endl;
    std::cout << "Auth log: " << config.log.auth_log.string() << std::endl;
    std::cout << "Log level: " << config.log.level << std::endl;
    std::cout << "Mount paths:" << std::endl;
    for (const auto& p : config.mount.paths) {
        std::cout << "  - " << p.string() << std::endl;
    }
    if (!config.ssh.default_keys.empty()) {
        std::cout << "SSH default keys:" << std::endl;
        for (const auto& k : config.ssh.default_keys) {
            std::string name = k.name.empty() ? "(unnamed)" : k.name;
            std::string key_preview = k.public_key.size() > 40
                ? k.public_key.substr(0, 40) + "..."
                : k.public_key;
            std::cout << "  - [" << name << "] " << key_preview << std::endl;
        }
    }
    std::cout << "Loaded: " << (config.loaded ? "yes" : "no (using defaults)") << std::endl;
    return 0;
}

}

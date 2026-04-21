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

    auto proj_opt = core::PathResolver::resolve(project_path);
    if (!proj_opt) {
        std::cerr << "Invalid project path: " << project_path << std::endl;
        return 1;
    }
    fs::path proj = *proj_opt;

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

    auto grp_result = utils::exec_safe({"usermod", "-aG", user_info.username, main_user});
    if (grp_result.exit_code != 0) {
        utils::get_logger()->warn("Failed to add {} to group {}: {}", main_user, user_info.username, grp_result.stderr_output);
    }

    int mount_failures = 0;

    ctx.ssh_mgr->set_key_path(ctx.config.ssh.key_path);
    ctx.ssh_mgr->set_key_type(ctx.config.ssh.key_type);

    if (!ctx.ssh_mgr->setup_passwordless(main_user, user_info.username)) {
        std::cerr << "Error: SSH setup failed, aborting" << std::endl;
        ctx.user_mgr->remove_ai_user(user_info.username, true);
        return 1;
    }

    if (!ctx.config.ssh.ai_default_key.empty()) {
        if (!ctx.ssh_mgr->setup_default_key_from_file(user_info.username, ctx.config.ssh.ai_default_key)) {
            utils::get_logger()->warn("Failed to authorize default key for {}", user_info.username);
        }
    }

    for (const auto& mount_path : ctx.config.mount.paths) {
        auto source_opt = core::PathResolver::resolve(mount_path.string());
        if (!source_opt) {
            utils::get_logger()->warn("Invalid mount path, skipping: {}", mount_path.string());
            continue;
        }
        fs::path source = *source_opt;
        if (!fs::exists(source)) {
            utils::get_logger()->warn("Mount source does not exist, skipping: {}", source.string());
            continue;
        }

        if (!utils::is_path_allowed(source, main_user)) {
            utils::get_logger()->error("Mount source path not allowed, skipping: {}", source.string());
            continue;
        }

        fs::path target = core::PathResolver::to_ai_user_path(source, user_info.username, main_user, user_info.home_dir);
        if (!ctx.graft->bind_mount(source, target, true, user_info.uid, user_info.gid)) {
            mount_failures++;
            utils::get_logger()->error("Mount failed: {} -> {}", source.string(), target.string());
        }
    }

    if (mount_failures > 0) {
        utils::get_logger()->warn("cmd_create completed with {} mount failure(s)", mount_failures);
        return 1;
    }

    if (!ctx.graft->grant_write_access(proj, user_info.username)) {
        utils::get_logger()->warn("Failed to grant write access to project: {}", proj.string());
    }

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
    std::string main_user = utils::get_effective_username();

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

    std::string username = state->username;
    std::string home_dir = state->home_dir;
    int fixes = 0;

    auto grp_result = utils::exec_safe({"usermod", "-aG", username, main_user});
    if (grp_result.exit_code == 0) {
        fixes++;
    } else {
        utils::get_logger()->warn("Failed to add {} to group {}: {}", main_user, username, grp_result.stderr_output);
    }

    ctx.ssh_mgr->set_key_path(ctx.config.ssh.key_path);
    ctx.ssh_mgr->set_key_type(ctx.config.ssh.key_type);

    fs::path auth_keys = fs::path(home_dir) / ".ssh" / "authorized_keys";
    if (!fs::exists(auth_keys)) {
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

    for (const auto& mount_path : ctx.config.mount.paths) {
        auto source_opt = core::PathResolver::resolve(mount_path.string());
        if (!source_opt) continue;
        fs::path source = *source_opt;
        if (!fs::exists(source)) continue;
        if (!utils::is_path_allowed(source, main_user)) continue;

        fs::path target = core::PathResolver::to_ai_user_path(source, username, main_user, home_dir);
        if (ctx.graft->is_mounted(target)) continue;

        utils::get_logger()->info("Fixing mount: {} -> {}", source.string(), target.string());
        if (ctx.graft->bind_mount(source, target, true, state->uid, state->gid)) {
            fixes++;
        } else {
            utils::get_logger()->warn("Failed to remount {} -> {}", source.string(), target.string());
        }
    }

    if (!ctx.graft->grant_write_access(proj, username)) {
        utils::get_logger()->warn("Failed to grant write access to: {}", proj.string());
    }

    utils::get_logger()->info("Update complete: {} fix(es) applied for {}", fixes, username);
    return 0;
}

}

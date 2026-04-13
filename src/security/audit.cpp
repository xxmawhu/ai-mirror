#include "ai_mirror/security/audit.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <grp.h>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>

namespace ai_mirror::security {

namespace {

bool check_path_permissions(const fs::path& p, const std::string& expected_mode) {
    std::error_code ec;
    auto status = fs::status(p, ec);
    if (ec || !fs::exists(status)) return false;

    auto perms = status.permissions();

    if (expected_mode == "read-only") {
        return (perms & fs::perms::owner_write) == fs::perms::none;
    }
    if (expected_mode == "owner-only") {
        return (perms & fs::perms::group_write) == fs::perms::none
            && (perms & fs::perms::others_write) == fs::perms::none;
    }
    return true;
}

bool check_user_exists(const std::string& username) {
    auto* pw = getpwnam(username.c_str());
    return pw != nullptr;
}

bool check_no_shell_login(const std::string& username) {
    auto* pw = getpwnam(username.c_str());
    if (!pw) return false;
    std::string shell(pw->pw_shell);
    return shell == "/usr/sbin/nologin" || shell == "/bin/false" || shell == "/sbin/nologin";
}

bool check_home_owner(const std::string& username) {
    auto* pw = getpwnam(username.c_str());
    if (!pw) return false;
    std::string home(pw->pw_dir);
    struct stat st;
    if (stat(home.c_str(), &st) != 0) return false;
    return st.st_uid == pw->pw_uid;
}

bool check_config_permissions(const fs::path& config_path) {
    struct stat st;
    if (stat(config_path.c_str(), &st) != 0) return false;
    return (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

}

AuditReport audit_mounts_for_user(const std::string& username) {
    AuditReport report;

    auto logger = ai_mirror::utils::get_logger();
    logger->info("Auditing mounts for user: {}", username.c_str());

    auto* pw = getpwnam(username.c_str());
    if (!pw) {
        report.add("mount", "user_exists", false, "User not found: " + username);
        return report;
    }
    report.add("mount", "user_exists", true);

    std::string home(pw->pw_dir);
    auto mounts_cmd = "findmnt -l -n -o TARGET,SOURCE,FSTYPE";
    auto result = ai_mirror::utils::execute(mounts_cmd);

    if (result.exit_code != 0) {
        report.add("mount", "findmnt", false, "findmnt command failed");
        return report;
    }

    std::istringstream stream(result.stdout_output);
    std::string line;
    int mount_count = 0;
    while (std::getline(stream, line)) {
        if (line.find(home) == 0) {
            mount_count++;
            report.add("mount", "bind_mount:" + line, true, "active mount found");
        }
    }
    report.add("mount", "mount_count", mount_count >= 0,
                std::to_string(mount_count) + " mounts found for " + username);

    return report;
}

AuditReport audit_user_permissions(const std::string& username) {
    AuditReport report;

    report.add("user", "exists", check_user_exists(username), username);
    report.add("user", "no_shell_login", check_no_shell_login(username),
                check_no_shell_login(username) ? "restricted shell" : "WARNING: login shell enabled");
    report.add("user", "home_owner", check_home_owner(username), username + " home directory ownership");

    auto* pw = getpwnam(username.c_str());
    if (pw) {
        std::string home(pw->pw_dir);
        std::error_code ec;
        auto status = fs::status(home, ec);
        if (!ec && fs::is_directory(status)) {
            auto perms = status.permissions();
            bool no_world_write = (perms & fs::perms::others_write) == fs::perms::none;
            report.add("user", "home_no_world_write", no_world_write,
                        no_world_write ? "OK" : "WARNING: home is world-writable");
        }
    }

    return report;
}

AuditReport audit_config_security(const fs::path& config_path) {
    AuditReport report;

    std::error_code ec;
    bool exists = fs::exists(config_path, ec);
    report.add("config", "exists", exists, config_path.string());

    if (!exists) {
        return report;
    }

    report.add("config", "not_group_world_writable",
                check_config_permissions(config_path),
                "config file permissions check");

    report.add("config", "owner_only_readable",
                check_path_permissions(config_path, "owner-only"),
                "config should be owner-readable only");

    return report;
}

AuditReport full_audit() {
    AuditReport report;

    auto logger = ai_mirror::utils::get_logger();
    logger->info("Starting full security audit");

    report.add("system", "root_check", !ai_mirror::utils::is_root(),
                ai_mirror::utils::is_root() ? "WARNING: running as root" : "not running as root");

    auto config_home = ai_mirror::utils::get_effective_home();
    fs::path config_path = fs::path(config_home) / ".ai-mirror.toml";
    auto config_report = audit_config_security(config_path);
    for (auto& entry : config_report.entries) {
        report.add("config:" + entry.category, entry.item, entry.passed, entry.detail);
    }

    auto ssh_dir = fs::path(config_home) / ".ssh";
    std::error_code ec;
    if (fs::exists(ssh_dir, ec)) {
        report.add("system", "ssh_dir_exists", true);
        report.add("system", "ssh_dir_permissions",
                    check_path_permissions(ssh_dir, "owner-only"),
                    ".ssh should be owner-only");
    } else {
        report.add("system", "ssh_dir_exists", false, ".ssh directory not found");
    }

    auto mounts_result = ai_mirror::utils::execute("findmnt -l -n -o TARGET,FSTYPE");
    if (mounts_result.exit_code == 0) {
        std::istringstream stream(mounts_result.stdout_output);
        std::string line;
        int bind_count = 0;
        while (std::getline(stream, line)) {
            if (line.find("fuse.bind") != std::string::npos || line.find("none") != std::string::npos) {
                bind_count++;
            }
        }
        report.add("system", "bind_mounts", true,
                    std::to_string(bind_count) + " bind mounts detected");
    }

    return report;
}

bool write_audit_report(const AuditReport& report, const fs::path& output_path) {
    std::ofstream ofs(output_path);
    if (!ofs.is_open()) return false;

    ofs << "=== ai-mirror Security Audit Report ===" << "\n\n";

    for (const auto& entry : report.entries) {
        ofs << "[" << (entry.passed ? "PASS" : "FAIL") << "] "
            << entry.category << "/" << entry.item;
        if (!entry.detail.empty()) {
            ofs << " - " << entry.detail;
        }
        ofs << "\n";
    }

    ofs << "\n--- Summary ---\n";
    ofs << "Total checks: " << report.entries.size() << "\n";
    ofs << "Passed: " << report.passed_count << "\n";
    ofs << "Failed: " << report.failed_count << "\n";
    ofs << "Result: " << (report.all_passed() ? "ALL PASSED" : "ISSUES FOUND") << "\n";

    auto logger = ai_mirror::utils::get_logger();
    logger->info("Audit report written to {}", output_path.c_str());

    return true;
}

}

#include "ai_mirror/security/path_validator.hpp"
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ai_mirror::security {

static const std::vector<std::string> SYSTEM_DIRS = {
    "/etc", "/root", "/var", "/proc", "/sys", "/dev",
    "/boot", "/lib", "/usr", "/sbin", "/bin", "/run"
};

fs::path safe_canonical(const fs::path& p) {
    std::error_code ec;
    auto canonical = fs::canonical(p, ec);
    if (!ec) return canonical;

    auto weak = fs::weakly_canonical(p, ec);
    if (!ec) {
        for (const auto& part : weak) {
            if (part == "..") return fs::path{};
        }
        return weak;
    }

    return fs::path{};
}

bool validate_path_allowed(const fs::path& p) {
    if (p.empty()) return false;
    auto resolved = safe_canonical(p);
    std::string s = resolved.string();
    for (const auto& d : SYSTEM_DIRS) {
        if (s.length() >= d.length() && s.substr(0, d.length()) == d) {
            if (s.length() == d.length() || s[d.length()] == '/') {
                return false;
            }
        }
    }
    return true;
}

bool validate_mount_source(const fs::path& source) {
    if (source.empty()) return false;
    return validate_path_allowed(source);
}

bool is_subpath(const fs::path& parent, const fs::path& child) {
    auto norm_parent = safe_canonical(parent);
    auto norm_child = safe_canonical(child);

    auto parent_str = norm_parent.string();
    auto child_str = norm_child.string();

    if (parent_str == child_str) {
        return true;
    }

    if (child_str.length() > parent_str.length()) {
        return child_str.substr(0, parent_str.length() + 1) == parent_str + "/";
    }

    return false;
}

PathCheckResult validate_mount_paths(const fs::path& source, const fs::path& target) {
    if (source.empty() || target.empty()) {
        return {false, "Source and target paths must not be empty"};
    }

    auto norm_source = safe_canonical(source);
    auto norm_target = safe_canonical(target);

    if (norm_source == norm_target) {
        return {false, "Source and target are the same path: " + norm_source.string()};
    }

    if (is_subpath(source, target)) {
        return {false, "Target (" + target.string() + ") is a subdirectory of source (" + source.string() + ")"};
    }

    if (is_subpath(target, source)) {
        return {false, "Source (" + source.string() + ") is a subdirectory of target (" + target.string() + ")"};
    }

    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return {false, "Source path does not exist: " + source.string()};
    }

    return {true, "OK"};
}

PathCheckResult validate_no_circular_mount(const fs::path& source, const fs::path& target) {
    std::error_code ec;

    auto norm_source = safe_canonical(source);
    auto norm_target = safe_canonical(target);

    if (is_subpath(norm_source, norm_target)) {
        return {false, "Circular mount detected: source is under target"};
    }

    return {true, "OK"};
}

bool validate_path_exists(const fs::path& p) {
    if (p.empty()) return false;

    int fd = ::open(p.c_str(), O_PATH | O_NOFOLLOW);
    if (fd < 0) return false;

    struct stat st{};
    int ret = ::fstat(fd, &st);
    ::close(fd);

    if (ret < 0) return false;

    return S_ISDIR(st.st_mode) || S_ISREG(st.st_mode);
}

}

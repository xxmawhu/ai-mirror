#include "ai_mirror/core/config.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/unique_fd.hpp"
#include <fstream>
#include <sstream>
#include <toml.hpp>
#include <sys/stat.h>
#include <fcntl.h>

namespace ai_mirror::core {

static constexpr size_t MAX_CONFIG_SIZE = 1048576;

static bool validate_config_file_security(const fs::path& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) {
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        utils::get_logger()->error("Config file is a symlink, rejecting: {}", p.string());
        return false;
    }
    uid_t expected_uid = utils::get_login_uid();
    if (expected_uid == 0) expected_uid = getuid();
    if (st.st_uid != expected_uid) {
        utils::get_logger()->error("Config file not owned by expected user (uid {} != {}), rejecting: {}",
            st.st_uid, expected_uid, p.string());
        return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        utils::get_logger()->warn("Config file is group/world writable: {}", p.string());
    }
    return true;
}

static std::string toml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

fs::path ConfigParser::resolve_home(const fs::path& p) {
    auto str = p.string();
    if (str == "~") {
        return fs::path(utils::get_effective_home());
    }
    if (str.size() >= 2 && str[0] == '~' && str[1] == '/') {
        return fs::path(utils::get_effective_home()) / str.substr(2);
    }
    if (str.size() > 1 && str[0] == '~') {
        auto slash_pos = str.find('/');
        std::string username = (slash_pos == std::string::npos)
            ? str.substr(1)
            : str.substr(1, slash_pos - 1);
        std::string rest = (slash_pos == std::string::npos)
            ? ""
            : str.substr(slash_pos + 1);
        std::string home = utils::get_home_dir(username);
        if (home.empty()) return p;
        return rest.empty() ? fs::path(home) : fs::path(home) / rest;
    }
    return p;
}

fs::path ConfigParser::expand_path(const std::string& path) {
    return resolve_home(fs::path(path));
}

Config ConfigParser::load(const fs::path& config_path) {
    Config config;
    config.config_path = config_path;

    std::error_code ec;
    auto file_size = fs::file_size(config_path, ec);
    if (ec || file_size > MAX_CONFIG_SIZE) {
        utils::get_logger()->error("Config file too large or inaccessible (size={}, max={}): {}",
            ec ? 0 : file_size, MAX_CONFIG_SIZE, config_path.string());
        return config;
    }

    try {
        auto data = toml::parse(config_path.string());

        if (data.as_table().contains("user")) {
            try {
                auto& user = data["user"];
                if (user.as_table().contains("prefix")) {
                    config.user.prefix = toml::get<std::string>(user["prefix"]);
                }
            } catch (const std::exception& e) {
                std::string field_err = std::string("user.prefix: ") + e.what();
                config.load_error += (config.load_error.empty() ? "" : "; ") + field_err;
                utils::get_logger()->warn("Config field error: {}", field_err);
            }
        }

        if (data.as_table().contains("mount")) {
            try {
                auto& mount = data["mount"];
                if (mount.as_table().contains("paths")) {
                    auto paths = toml::get<std::vector<std::string>>(mount["paths"]);
                    for (const auto& p : paths) {
                        config.mount.paths.push_back(expand_path(p));
                    }
                }
            } catch (const std::exception& e) {
                std::string field_err = std::string("mount.paths: ") + e.what();
                config.load_error += (config.load_error.empty() ? "" : "; ") + field_err;
                utils::get_logger()->warn("Config field error: {}", field_err);
            }
        }

        if (data.as_table().contains("ssh")) {
            try {
                auto& ssh = data["ssh"];
                if (ssh.as_table().contains("key_type")) {
                    config.ssh.key_type = toml::get<std::string>(ssh["key_type"]);
                }
                if (ssh.as_table().contains("key_path")) {
                    config.ssh.key_path = expand_path(toml::get<std::string>(ssh["key_path"]));
                } else {
                    config.ssh.key_path = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
                }
                if (ssh.as_table().contains("ai_default_key")) {
                    config.ssh.ai_default_key = expand_path(toml::get<std::string>(ssh["ai_default_key"]));
                }
            } catch (const std::exception& e) {
                std::string field_err = std::string("ssh: ") + e.what();
                config.load_error += (config.load_error.empty() ? "" : "; ") + field_err;
                utils::get_logger()->warn("Config field error: {}", field_err);
            }
        }

        config.loaded = true;
    } catch (const std::exception& e) {
        config.load_error = std::string("TOML parse error: ") + e.what();
        utils::get_logger()->warn("Failed to parse config: {}, using defaults", config.load_error);
    } catch (...) {
        config.load_error = "Unknown error during config parsing";
        utils::get_logger()->warn("Failed to parse config: unknown error, using defaults");
    }

    return config;
}

// Atomic config file creation with TOCTOU protection:
// 1. O_CREAT|O_EXCL: Rejects if file already exists (prevents pre-placement attacks)
// 2. O_NOFOLLOW: Rejects symlink targets (prevents symlink attacks)
// 3. EEXIST case: Verify file exists rather than blindly trusting errno
// 4. Cleanup on write failure: Remove newly created empty file
static bool try_auto_create_config(const fs::path& config_path) {
    auto parent = config_path.parent_path();
    if (!parent.empty()) {
        if (!security::safe_create_directories(parent)) {
            utils::get_logger()->warn("Failed to create parent directory: {}", parent.string());
            return false;
        }
    }

    utils::unique_fd ufd(open(config_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    if (!ufd) {
        if (errno == EEXIST) {
            return fs::exists(config_path);
        }
        utils::get_logger()->warn("Failed to create config (O_EXCL|O_NOFOLLOW): {}", config_path.string());
        return false;
    }
    ufd.reset();

    uid_t login_uid = utils::get_login_uid();
    if (login_uid != 0) {
        if (chown(config_path.c_str(), login_uid, (gid_t)-1) != 0) {
            utils::get_logger()->warn("Failed to chown config to uid {}: {}", login_uid, strerror(errno));
        }
    }

    auto default_cfg = ConfigParser::create_default_config(config_path);
    if (!ConfigParser::save(default_cfg, config_path)) {
        utils::get_logger()->warn("Failed to auto-create config: {}", config_path.string());
        fs::remove(config_path);
        return false;
    }

    utils::get_logger()->info("Auto-created config: {}", config_path.string());
    return true;
}

Config ConfigParser::load_default() {
    fs::path default_path = fs::path(utils::get_effective_home()) / ".ai-mirror.toml";

    try_auto_create_config(default_path);

    if (fs::exists(default_path)) {
        if (!validate_config_file_security(default_path)) {
            utils::get_logger()->error("Config file security validation failed, using defaults");
            Config config;
            config.config_path = default_path;
            config.ssh.key_path = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
            config.loaded = false;
            return config;
        }
        return load(default_path);
    }

    Config config;
    config.config_path = default_path;
    config.ssh.key_path = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
    config.loaded = false;

    return config;
}

Config ConfigParser::create_default_config(const fs::path& config_path) {
    Config config;
    config.config_path = config_path;
    config.mount.paths = {
        fs::path("~/.bashrc"),
        fs::path("~/.config"),
    };
    config.ssh.key_type = "ed25519";
    config.ssh.key_path = fs::path("~/.ssh/ai-mirror");
    config.ssh.ai_default_key = fs::path("~/.ssh/id_ed25519");

    return config;
}

bool ConfigParser::save(const Config& config, const fs::path& config_path) {
    struct stat st;
    if (lstat(config_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        utils::get_logger()->error("Config path is a symlink, rejecting: {}", config_path.string());
        return false;
    }

    fs::path tmp_path = config_path;
    tmp_path += ".tmp";

    utils::unique_fd ufd(::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!ufd) {
        if (errno == EEXIST) {
            fs::remove(tmp_path);
            ufd.reset(::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        }
        if (!ufd) {
            utils::get_logger()->error("Failed to create temp config file: {}", tmp_path.string());
            return false;
        }
    }

    std::ostringstream oss;
    oss << "[mount]\n"
        << "paths = [\n";
    for (const auto& p : config.mount.paths) {
        oss << "    \"" << toml_escape(p.string()) << "\",\n";
    }
    oss << "]\n\n"
        << "[ssh]\n"
        << "key_type = \"" << toml_escape(config.ssh.key_type) << "\"\n"
        << "key_path = \"" << toml_escape(config.ssh.key_path.string()) << "\"\n"
        << "ai_default_key = \"" << toml_escape(config.ssh.ai_default_key.string()) << "\"\n";

    std::string content = oss.str();
    ssize_t written = ::write(ufd.get(), content.c_str(), content.size());
    ufd.reset();

    if (written != static_cast<ssize_t>(content.size())) {
        utils::get_logger()->error("Failed to write config content: {}", tmp_path.string());
        fs::remove(tmp_path);
        return false;
    }

    std::error_code ec;
    fs::rename(tmp_path, config_path, ec);
    if (ec) {
        utils::get_logger()->error("Atomic rename failed for config: {} -> {}: {}", tmp_path.string(), config_path.string(), ec.message());
        fs::remove(tmp_path);
        return false;
    }

    return true;
}

}

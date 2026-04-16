#include "ai_mirror/core/config.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <fstream>
#include <sstream>
#include <toml.hpp>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace ai_mirror::core {

static bool validate_config_file_security(const fs::path& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) {
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        utils::get_logger()->error("Config file is a symlink, rejecting: {}", p.string());
        return false;
    }
    if (st.st_uid != getuid()) {
        utils::get_logger()->error("Config file not owned by current user (uid {} != {}), rejecting: {}",
            st.st_uid, getuid(), p.string());
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

    try {
        auto data = toml::parse(config_path.string());

        if (data.as_table().contains("mount")) {
            auto& mount = data["mount"];
            if (mount.as_table().contains("paths")) {
                auto paths = toml::get<std::vector<std::string>>(mount["paths"]);
                for (const auto& p : paths) {
                    config.mount.paths.push_back(expand_path(p));
                }
            }
        }

        if (data.as_table().contains("ssh")) {
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
        }

        config.loaded = true;
    } catch (const std::exception& e) {
        std::string msg = e.what();
        utils::get_logger()->warn(std::string("Failed to load config: ") + msg + ", using defaults");
    }

    return config;
}

static bool try_auto_create_config(const fs::path& config_path) {
    auto parent = config_path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !fs::exists(parent, ec)) {
        fs::create_directories(parent, ec);
    }

    int fd = open(config_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return fs::exists(config_path);
        }
        utils::get_logger()->warn("Failed to create config (O_EXCL|O_NOFOLLOW): {}", config_path.string());
        return false;
    }
    close(fd);

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
    config.ssh.ai_default_key = fs::path("~/.ssh/id_ed25519.pub");

    return config;
}

bool ConfigParser::save(const Config& config, const fs::path& config_path) {
    struct stat st;
    if (lstat(config_path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        utils::get_logger()->error("Config path is a symlink, rejecting: {}", config_path.string());
        return false;
    }

    int fd = open(config_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        utils::get_logger()->error("Failed to open config for writing: {}", config_path.string());
        return false;
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
    ssize_t written = write(fd, content.c_str(), content.size());
    close(fd);

    if (written != static_cast<ssize_t>(content.size())) {
        utils::get_logger()->error("Failed to write config content: {}", config_path.string());
        return false;
    }

    return true;
}

}

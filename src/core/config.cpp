#include "ai_mirror/core/config.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <fstream>
#include <toml.hpp>

namespace ai_mirror::core {

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

        if (data.as_table().contains("user")) {
            auto& user = data["user"];
            if (user.as_table().contains("prefix")) {
                config.user.prefix = toml::get<std::string>(user["prefix"]);
            }
        }

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
            if (ssh.as_table().contains("default_keys")) {
                auto& keys = ssh["default_keys"];
                auto& keys_arr = keys.as_array();
                for (size_t i = 0; i < keys_arr.size(); ++i) {
                    auto& entry = keys_arr.at(i);
                    auto& entry_table = entry.as_table();
                    SSHKeyEntry key_entry;
                    if (entry_table.contains("name")) {
                        key_entry.name = toml::get<std::string>(entry_table["name"]);
                    }
                    if (entry_table.contains("public_key")) {
                        key_entry.public_key = toml::get<std::string>(entry_table["public_key"]);
                    }
                    if (!key_entry.public_key.empty()) {
                        config.ssh.default_keys.push_back(std::move(key_entry));
                    }
                }
            }
        }

        if (data.as_table().contains("log")) {
            auto& log_sec = data["log"];
            if (log_sec.as_table().contains("auth_log")) {
                config.log.auth_log = toml::get<std::string>(log_sec["auth_log"]);
            }
            if (log_sec.as_table().contains("level")) {
                config.log.level = toml::get<std::string>(log_sec["level"]);
            }
        }

        config.loaded = true;
    } catch (const std::exception& e) {
        std::string msg = e.what();
        utils::get_logger()->warn(std::string("Failed to load config: ") + msg + ", using defaults");
    }

    return config;
}

Config ConfigParser::load_default() {
    fs::path default_path = fs::path(utils::get_effective_home()) / ".ai-mirror.toml";

    if (fs::exists(default_path)) {
        return load(default_path);
    }

    Config config;
    config.config_path = default_path;
    config.user.prefix = "i";
    config.ssh.key_path = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
    config.loaded = false;

    return config;
}

Config ConfigParser::create_default_config(const fs::path& config_path) {
    Config config;
    config.config_path = config_path;
    config.user.prefix = "i";
    config.mount.paths = {
        fs::path("~/.bashrc"),
        fs::path("~/.config"),
    };
    config.ssh.key_type = "ed25519";
    config.ssh.key_path = fs::path("~/.ssh/ai-mirror");
    config.log.auth_log = "/var/log/auth.log";
    config.log.level = "info";

    return config;
}

bool ConfigParser::save(const Config& config, const fs::path& config_path) {
    std::ofstream ofs(config_path);
    if (!ofs.is_open()) {
        return false;
    }

    ofs << "[user]\n"
        << "prefix = \"" << config.user.prefix << "\"\n\n"
        << "[mount]\n"
        << "paths = [\n";
    for (const auto& p : config.mount.paths) {
        ofs << "    \"" << p.string() << "\",\n";
    }
    ofs << "]\n\n"
        << "[ssh]\n"
        << "key_type = \"" << config.ssh.key_type << "\"\n"
        << "key_path = \"" << config.ssh.key_path.string() << "\"\n";

    for (const auto& k : config.ssh.default_keys) {
        ofs << "\n[[ssh.default_keys]]\n"
            << "name = \"" << k.name << "\"\n"
            << "public_key = \"" << k.public_key << "\"\n";
    }

    ofs << "\n[log]\n"
        << "auth_log = \"" << config.log.auth_log.string() << "\"\n"
        << "level = \"" << config.log.level << "\"\n";

    return true;
}

}

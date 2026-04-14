#include "ai_mirror/core/config.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <fstream>
#include <toml.hpp>

namespace ai_mirror::core {

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
    if (fs::exists(config_path)) return true;

    auto default_cfg = ConfigParser::create_default_config(config_path);
    if (!ConfigParser::save(default_cfg, config_path)) {
        utils::get_logger()->warn("Failed to auto-create config: {}", config_path.string());
        return false;
    }

    utils::get_logger()->info("Auto-created config: {}", config_path.string());
    return true;
}

Config ConfigParser::load_default() {
    fs::path default_path = fs::path(utils::get_effective_home()) / ".ai-mirror.toml";

    try_auto_create_config(default_path);

    if (fs::exists(default_path)) {
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
    std::ofstream ofs(config_path);
    if (!ofs.is_open()) {
        return false;
    }

    ofs << "[mount]\n"
        << "paths = [\n";
    for (const auto& p : config.mount.paths) {
        ofs << "    \"" << toml_escape(p.string()) << "\",\n";
    }
    ofs << "]\n\n"
        << "[ssh]\n"
        << "key_type = \"" << toml_escape(config.ssh.key_type) << "\"\n"
        << "key_path = \"" << toml_escape(config.ssh.key_path.string()) << "\"\n"
        << "ai_default_key = \"" << toml_escape(config.ssh.ai_default_key.string()) << "\"\n";

    return true;
}

}

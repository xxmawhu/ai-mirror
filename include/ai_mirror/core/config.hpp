#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

struct SSHKeyEntry {
    std::string name;
    std::string public_key;
};

struct Config {
    struct UserConfig {
        std::string prefix = "i";
        std::vector<fs::path> allowed_bases;  // extra allowed base paths beyond $HOME (e.g. BeeGFS mount points)
    } user;

    struct MountConfig {
        std::vector<fs::path> paths;
    } mount;

    struct SSHConfig {
        std::string key_type = "ed25519";
        fs::path key_path;
        fs::path ai_default_key;
    } ssh;

    fs::path config_path;
    bool loaded = false;
    std::string load_error;
};

class ConfigParser {
public:
    static Config load(const fs::path& config_path);
    static Config load_default();
    static bool save(const Config& config, const fs::path& config_path);
    static Config create_default_config(const fs::path& config_path);

private:
    static fs::path expand_path(const std::string& path);
    static fs::path resolve_home(const fs::path& path);
};

}

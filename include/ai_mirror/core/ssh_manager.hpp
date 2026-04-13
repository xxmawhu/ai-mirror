#pragma once

#include "ai_mirror/core/config.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

class SSHManager {
public:
    SSHManager();

    bool generate_key_pair(const fs::path& key_path, const std::string& key_type = "ed25519");
    bool authorize_key(const std::string& username, const fs::path& public_key_path);
    bool authorize_public_key_string(const std::string& username, const std::string& public_key);
    bool setup_passwordless(const std::string& main_user, const std::string& ai_user);
    bool setup_default_keys(const std::string& ai_user, const std::vector<SSHKeyEntry>& default_keys);
    fs::path get_public_key_path(const fs::path& key_path) const;
    bool test_connection(const std::string& username) const;

    void set_key_type(const std::string& type) { key_type_ = type; }
    void set_key_path(const fs::path& path) { key_path_ = path; }

private:
    std::string key_type_ = "ed25519";
    fs::path key_path_;
    bool ensure_ssh_dir(const std::string& username);
};

}

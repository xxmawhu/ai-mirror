#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <fstream>

namespace ai_mirror::core {

SSHManager::SSHManager() {
    key_path_ = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
}

bool SSHManager::generate_key_pair(const fs::path& key_path, const std::string& key_type) {
    fs::path ssh_dir = key_path.parent_path();
    std::error_code ec;
    if (!fs::exists(ssh_dir, ec)) {
        fs::create_directories(ssh_dir, ec);
        utils::execute("chmod 700 " + ssh_dir.string());
    }

    if (fs::exists(key_path, ec)) {
        utils::get_logger()->info("SSH key already exists: {}", key_path.c_str());
        return true;
    }

    std::ostringstream cmd;
    cmd << "ssh-keygen -t " << key_type
        << " -f " << key_path.string()
        << " -N '' -C 'ai-mirror' -q";

    auto result = utils::execute(cmd.str());
    if (result.exit_code != 0) {
        utils::get_logger()->error("ssh-keygen failed: {}", result.stderr_output.c_str());
        return false;
    }

    utils::get_logger()->info("Generated SSH key pair: {}", key_path.c_str());
    return true;
}

bool SSHManager::ensure_ssh_dir(const std::string& username) {
    std::string home = utils::get_home_dir(username);
    fs::path ssh_dir = fs::path(home) / ".ssh";

    std::error_code ec;
    if (!fs::exists(ssh_dir, ec)) {
        std::ostringstream cmd;
        cmd << "mkdir -p " << ssh_dir.string()
            << " && chown " << username << ":" << username << " " << ssh_dir.string()
            << " && chmod 700 " << ssh_dir.string();

        auto result = utils::execute(cmd.str());
        if (result.exit_code != 0) {
            utils::get_logger()->error("Failed to create .ssh dir for {}", username.c_str());
            return false;
        }
    }
    return true;
}

bool SSHManager::authorize_key(const std::string& username, const fs::path& public_key_path) {
    if (!ensure_ssh_dir(username)) {
        return false;
    }

    std::string home = utils::get_home_dir(username);
    fs::path auth_keys = fs::path(home) / ".ssh" / "authorized_keys";

    std::ostringstream cmd;
    cmd << "cat " << public_key_path.string()
        << " >> " << auth_keys.string()
        << " && chown " << username << ":" << username << " " << auth_keys.string()
        << " && chmod 600 " << auth_keys.string();

    auto result = utils::execute(cmd.str());
    if (result.exit_code != 0) {
        utils::get_logger()->error("Failed to authorize key for {}", username.c_str());
        return false;
    }

    utils::get_logger()->info("Authorized key for {}", username.c_str());
    return true;
}

bool SSHManager::authorize_public_key_string(const std::string& username, const std::string& public_key) {
    if (!ensure_ssh_dir(username)) {
        return false;
    }

    std::string home = utils::get_home_dir(username);
    fs::path auth_keys = fs::path(home) / ".ssh" / "authorized_keys";

    std::ostringstream cmd;
    cmd << "grep -qF '" << public_key << "' " << auth_keys.string()
        << " 2>/dev/null || echo '" << public_key << "' >> " << auth_keys.string()
        << " && chown " << username << ":" << username << " " << auth_keys.string()
        << " && chmod 600 " << auth_keys.string();

    auto result = utils::execute(cmd.str());
    if (result.exit_code != 0) {
        utils::get_logger()->error("Failed to authorize key string for {}", username.c_str());
        return false;
    }

    utils::get_logger()->info("Authorized inline key for {}", username.c_str());
    return true;
}

bool SSHManager::setup_default_keys(const std::string& ai_user, const std::vector<SSHKeyEntry>& default_keys) {
    for (const auto& entry : default_keys) {
        if (entry.public_key.empty()) continue;
        if (!authorize_public_key_string(ai_user, entry.public_key)) {
            std::string name = entry.name.empty() ? std::string("(unnamed)") : entry.name;
            utils::get_logger()->warn(std::string("Failed to authorize default key '") + name + "' for " + ai_user);
        }
    }
    return true;
}

bool SSHManager::setup_passwordless(const std::string& main_user, const std::string& ai_user) {
    fs::path pub_key = get_public_key_path(key_path_);

    if (!generate_key_pair(key_path_, key_type_)) {
        return false;
    }

    if (!authorize_key(ai_user, pub_key)) {
        return false;
    }

    utils::get_logger()->info("Passwordless SSH setup: {} -> {}", main_user.c_str(), ai_user.c_str());
    return true;
}

fs::path SSHManager::get_public_key_path(const fs::path& key_path) const {
    return fs::path(key_path.string() + ".pub");
}

bool SSHManager::test_connection(const std::string& username) const {
    std::ostringstream cmd;
    cmd << "ssh -o BatchMode=yes -o ConnectTimeout=5"
        << " -i " << key_path_.string()
        << " " << username << "@localhost 'echo ok' 2>&1";

    auto result = utils::execute(cmd.str());
    return result.exit_code == 0;
}

}

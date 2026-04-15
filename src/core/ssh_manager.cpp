#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <fstream>
#include <random>

namespace ai_mirror::core {

namespace {
fs::path make_random_tmp_path(const fs::path& base) {
    std::random_device rd;
    std::uniform_int_distribution<unsigned long> dist(0, ULONG_MAX);
    std::string suffix = ".tmp." + std::to_string(dist(rd));
    fs::path tmp = base;
    tmp += suffix;
    return tmp;
}
}

SSHManager::SSHManager() {
    key_path_ = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
}

bool SSHManager::generate_key_pair(const fs::path& key_path, const std::string& key_type) {
    if (!utils::validate_key_type(key_type)) {
        utils::get_logger()->error("Invalid SSH key type: {}", key_type);
        return false;
    }

    fs::path ssh_dir = key_path.parent_path();
    std::error_code ec;
    if (!fs::exists(ssh_dir, ec)) {
        fs::create_directories(ssh_dir, ec);
        auto r = utils::exec_safe({"chmod", "700", ssh_dir.string()});
        if (r.exit_code != 0) {
            utils::get_logger()->error("chmod 700 failed: {}", r.stderr_output);
        }
    }

    if (fs::exists(key_path, ec)) {
        utils::get_logger()->info("SSH key already exists: {}", key_path.c_str());
        return true;
    }

    auto result = utils::exec_safe({"ssh-keygen", "-t", key_type,
        "-f", key_path.string(), "-N", "", "-C", "ai-mirror", "-q"});
    if (result.exit_code != 0) {
        utils::get_logger()->error("ssh-keygen failed: {}", result.stderr_output.c_str());
        return false;
    }

    utils::get_logger()->info("Generated SSH key pair: {}", key_path.c_str());
    return true;
}

bool SSHManager::ensure_ssh_dir(const std::string& username) {
    if (!utils::validate_username(username)) {
        utils::get_logger()->error("Invalid username for SSH dir: {}", username);
        return false;
    }

    std::string home = utils::get_home_dir(username);
    fs::path ssh_dir = fs::path(home) / ".ssh";

    std::error_code ec;
    if (!fs::exists(ssh_dir, ec)) {
        auto r1 = utils::exec_safe({"mkdir", "-p", ssh_dir.string()});
        if (r1.exit_code != 0) {
            utils::get_logger()->error("Failed to create .ssh dir for {}", username.c_str());
            return false;
        }
        utils::exec_safe({"chown", username + ":" + username, ssh_dir.string()});
        auto r2 = utils::exec_safe({"chmod", "700", ssh_dir.string()});
        if (r2.exit_code != 0) {
            utils::get_logger()->error("chmod 700 .ssh failed for {}", username.c_str());
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

    std::string key_content;
    {
        std::ifstream ifs(public_key_path);
        std::string raw((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        if (raw.empty()) {
            utils::get_logger()->error("Public key file is empty: {}", public_key_path.string());
            return false;
        }
        while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) {
            raw.pop_back();
        }
        key_content = raw;
    }

    std::vector<std::string> existing_lines;
    {
        std::ifstream ifs(auth_keys);
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty()) existing_lines.push_back(line);
        }
    }

    for (const auto& l : existing_lines) {
        if (l == key_content) {
            utils::get_logger()->info("Key already authorized for {}", username);
            return true;
        }
    }

    existing_lines.push_back(key_content);

    fs::path tmp_path = make_random_tmp_path(auth_keys);
    {
        std::ofstream ofs(tmp_path, std::ios::trunc);
        if (!ofs.is_open()) {
            utils::get_logger()->error("Cannot open temp file for {}", username);
            return false;
        }
        for (const auto& l : existing_lines) {
            ofs << l << "\n";
        }
        ofs.flush();
        if (!ofs.good()) {
            utils::get_logger()->error("Write to temp file failed for {}", username);
            fs::remove(tmp_path);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp_path, auth_keys, ec);
    if (ec) {
        utils::get_logger()->error("Atomic rename failed for {}: {}", username, ec.message());
        fs::remove(tmp_path);
        return false;
    }

    utils::exec_safe({"chown", username + ":" + username, auth_keys.string()});
    auto chmod_r = utils::exec_safe({"chmod", "600", auth_keys.string()});
    if (chmod_r.exit_code != 0) {
        utils::get_logger()->error("chmod 600 authorized_keys failed for {}", username);
        return false;
    }

    utils::get_logger()->info("Authorized key for {}", username);
    return true;
}

bool SSHManager::authorize_public_key_string(const std::string& username, const std::string& public_key) {
    if (!utils::validate_ssh_public_key(public_key)) {
        utils::get_logger()->error("Invalid SSH public key format for {}", username);
        return false;
    }
    if (!ensure_ssh_dir(username)) {
        return false;
    }

    std::string home = utils::get_home_dir(username);
    fs::path auth_keys = fs::path(home) / ".ssh" / "authorized_keys";

    std::vector<std::string> existing_lines;
    {
        std::ifstream ifs(auth_keys);
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty()) existing_lines.push_back(line);
        }
    }

    for (const auto& l : existing_lines) {
        if (l == public_key) {
            utils::get_logger()->info("Key already authorized for {}", username);
            return true;
        }
    }

    existing_lines.push_back(public_key);

    fs::path tmp_path = make_random_tmp_path(auth_keys);
    {
        std::ofstream ofs(tmp_path, std::ios::trunc);
        if (!ofs.is_open()) {
            utils::get_logger()->error("Cannot open temp file for {}", username);
            return false;
        }
        for (const auto& l : existing_lines) {
            ofs << l << "\n";
        }
        ofs.flush();
        if (!ofs.good()) {
            utils::get_logger()->error("Write to temp file failed for {}", username);
            fs::remove(tmp_path);
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp_path, auth_keys, ec);
    if (ec) {
        utils::get_logger()->error("Atomic rename failed for {}: {}", username, ec.message());
        fs::remove(tmp_path);
        return false;
    }

    utils::exec_safe({"chown", username + ":" + username, auth_keys.string()});
    auto chmod_r = utils::exec_safe({"chmod", "600", auth_keys.string()});
    if (chmod_r.exit_code != 0) {
        utils::get_logger()->error("chmod 600 failed for {}", username);
        return false;
    }

    utils::get_logger()->info("Authorized inline key for {}", username);
    return true;
}

bool SSHManager::setup_default_key_from_file(const std::string& ai_user, const fs::path& public_key_path) {
    if (public_key_path.empty()) {
        utils::get_logger()->info("No ai_default_key configured, skipping default key setup");
        return true;
    }

    std::error_code ec;
    if (!fs::exists(public_key_path, ec)) {
        utils::get_logger()->warn("Default public key file not found: {}", public_key_path.string());
        return false;
    }

    if (!authorize_key(ai_user, public_key_path)) {
        utils::get_logger()->warn("Failed to authorize default key from file for {}", ai_user);
        return false;
    }

    utils::get_logger()->info("Authorized default key from {} for {}", public_key_path.string(), ai_user);
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
    if (!utils::validate_username(username)) return false;

    auto result = utils::exec_safe({"ssh", "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=5",
        "-i", key_path_.string(),
        username + "@localhost", "echo ok"});
    return result.exit_code == 0;
}

}

#pragma once

#include "ai_mirror/core/config.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

// SSH key management for passwordless access from main user to ai-user.
// All tempfile operations use safe_write_temp_file() which creates files
// with O_CREAT|O_EXCL|O_NOFOLLOW to prevent symlink/TOCTOU attacks.
// Random suffix generated from /dev/urandom (64-bit entropy) prevents
// prediction-based symlink placement attacks.
class SSHManager {
public:
    SSHManager();

    // Generate SSH key pair at specified path. Uses ssh-keygen with
    // specified key type (ed25519, rsa, ecdsa).
    bool generate_key_pair(const fs::path& key_path, const std::string& key_type = "ed25519");

    // Authorize a public key from file for the specified ai-user.
    // Public key file is validated with validate_ssh_public_key() and
    // read safely using lstat + O_NOFOLLOW to prevent symlink attacks.
    // Existing authorized_keys content is preserved and deduplicated.
    bool authorize_key(const std::string& username, const fs::path& public_key_path);

    // Authorize a public key string directly for the specified ai-user.
    // Key format validated before writing. Tempfile created with
    // O_EXCL|O_NOFOLLOW, then atomically renamed to final location.
    bool authorize_public_key_string(const std::string& username, const std::string& public_key);

    // Setup passwordless SSH from main_user to ai_user.
    bool setup_passwordless(const std::string& main_user, const std::string& ai_user);

    // Setup default key from configured ai_default_key file path.
    bool setup_default_key_from_file(const std::string& ai_user, const fs::path& public_key_path);

    // Setup multiple default keys from SSHKeyEntry vector.
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

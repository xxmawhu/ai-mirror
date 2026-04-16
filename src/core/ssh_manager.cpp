#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <array>
#include <sstream>
#include <iomanip>

namespace ai_mirror::core {

namespace {
// Reads cryptographic-quality random bytes from /dev/urandom and returns them
// as a hex string.  Using /dev/urandom (instead of std::random_device) avoids
// potential fallback to a PRNG on some platforms and guarantees kernel CSPRNG
// output.  The hex encoding provides 4 bits of entropy per character, so 8
// bytes (16 hex chars) yields 64 bits of entropy — sufficient to make
// prediction of a temp-file name computationally infeasible and prevent
// symlink / TOCTOU attacks on the temporary authorized_keys file.
std::string get_crypto_random_hex(size_t bytes) {
    std::array<unsigned char, 16> buf{};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        utils::get_logger()->error("Failed to open /dev/urandom");
        return "";
    }
    ssize_t n = read(fd, buf.data(), bytes);
    close(fd);
    if (n != static_cast<ssize_t>(bytes)) {
        utils::get_logger()->error("Failed to read from /dev/urandom");
        return "";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < bytes; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(buf[i]);
    }
    return oss.str();
}

fs::path make_random_tmp_path(const fs::path& base) {
    std::string suffix = ".tmp." + get_crypto_random_hex(8);
    fs::path tmp = base;
    tmp += suffix;
    return tmp;
}

bool safe_write_temp_file(const fs::path& path, const std::vector<std::string>& lines) {
    int fd = -1;
    fs::path tmp_path;
    for (int attempt = 0; attempt < 3; ++attempt) {
        tmp_path = make_random_tmp_path(path);
        fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) {
            utils::get_logger()->error("safe_write_temp_file: open({}) failed: {}",
                tmp_path.c_str(), strerror(errno));
            return false;
        }
    }
    if (fd < 0) {
        utils::get_logger()->error("safe_write_temp_file: exhausted retries for {}", path.c_str());
        return false;
    }

    FILE* f = ::fdopen(fd, "w");
    if (!f) {
        ::close(fd);
        fs::remove(tmp_path);
        utils::get_logger()->error("safe_write_temp_file: fdopen failed for {}", tmp_path.c_str());
        return false;
    }
    for (const auto& l : lines) {
        if (::fputs(l.c_str(), f) == EOF || ::fputc('\n', f) == EOF) {
            ::fclose(f);
            fs::remove(tmp_path);
            utils::get_logger()->error("safe_write_temp_file: write failed for {}", tmp_path.c_str());
            return false;
        }
    }
    if (::fflush(f) != 0) {
        ::fclose(f);
        fs::remove(tmp_path);
        utils::get_logger()->error("safe_write_temp_file: fflush failed for {}", tmp_path.c_str());
        return false;
    }
    ::fclose(f);
    return true;
}

bool safe_read_authorized_keys(const fs::path& path, std::vector<std::string>& lines_out) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        utils::get_logger()->error("safe_read_authorized_keys: lstat({}) failed: {}",
            path.c_str(), strerror(errno));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        utils::get_logger()->error("safe_read_authorized_keys: rejecting symlink: {}", path.c_str());
        return false;
    }

    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        utils::get_logger()->error("safe_read_authorized_keys: open({}) failed: {}",
            path.c_str(), strerror(errno));
        return false;
    }

    char buf[4096];
    std::string content;
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        content.append(buf, n);
    }
    ::close(fd);

    if (n < 0) {
        utils::get_logger()->error("safe_read_authorized_keys: read failed: {}", strerror(errno));
        return false;
    }

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            lines_out.push_back(line);
        }
    }
    return true;
}

bool safe_read_public_key_file(const fs::path& path, std::string& content_out, uid_t expected_uid) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        utils::get_logger()->error("safe_read_public_key_file: lstat({}) failed: {}",
            path.c_str(), strerror(errno));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        utils::get_logger()->error("safe_read_public_key_file: rejecting symlink: {}", path.c_str());
        return false;
    }
    if (st.st_uid != expected_uid) {
        utils::get_logger()->error("safe_read_public_key_file: ownership mismatch (uid {} != {}): {}",
            st.st_uid, expected_uid, path.c_str());
        return false;
    }

    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        utils::get_logger()->error("safe_read_public_key_file: open({}) failed: {}",
            path.c_str(), strerror(errno));
        return false;
    }

    char buf[4096];
    std::string content;
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        content.append(buf, n);
    }
    ::close(fd);

    if (n < 0) {
        utils::get_logger()->error("safe_read_public_key_file: read failed: {}", strerror(errno));
        return false;
    }

    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }

    if (content.empty()) {
        utils::get_logger()->error("safe_read_public_key_file: file is empty: {}", path.c_str());
        return false;
    }

    content_out = std::move(content);
    return true;
}
}

SSHManager::SSHManager() {
    key_path_ = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
}

// Generates or validates an existing SSH key pair.  When a key already exists
// at key_path, validates ownership (must be current user), rejects symlinks,
// warns on unsafe permissions (!=0600), and verifies the public key format via
// validate_ssh_public_key().  This prevents an attacker from pre-placing a
// malicious key or replacing the key with a symlink to a system file.
bool SSHManager::generate_key_pair(const fs::path& key_path, const std::string& key_type) {
    if (!utils::validate_key_type(key_type)) {
        utils::get_logger()->error("Invalid SSH key type: {}", key_type);
        return false;
    }

    fs::path ssh_dir = key_path.parent_path();
    std::error_code ec;
    if (!fs::exists(ssh_dir, ec)) {
        if (!security::safe_create_directories(ssh_dir)) {
            utils::get_logger()->error("Failed to create SSH directory: {}", ssh_dir.c_str());
            return false;
        }
        auto r = utils::exec_safe({"chmod", "700", ssh_dir.string()});
        if (r.exit_code != 0) {
            utils::get_logger()->error("chmod 700 failed: {}", r.stderr_output);
        }
    }

    if (fs::exists(key_path, ec)) {
        struct stat st;
        if (lstat(key_path.c_str(), &st) != 0) {
            utils::get_logger()->error("Cannot stat existing key: {}", key_path.c_str());
            return false;
        }
        if (S_ISLNK(st.st_mode)) {
            utils::get_logger()->error("SSH key is a symlink, rejecting: {}", key_path.c_str());
            return false;
        }
        if (st.st_uid != getuid()) {
            utils::get_logger()->error("SSH key not owned by current user (uid {} != {}), rejecting: {}",
                st.st_uid, getuid(), key_path.c_str());
            return false;
        }
        if ((st.st_mode & 0777) != 0600) {
            utils::get_logger()->warn("SSH private key has unsafe permissions {:o}, expected 0600: {}",
                st.st_mode & 0777, key_path.c_str());
        }

        fs::path pub_path = fs::path(key_path.string() + ".pub");
        if (fs::exists(pub_path, ec)) {
            std::ifstream ifs(pub_path);
            std::string pub_key((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
            while (!pub_key.empty() && (pub_key.back() == '\n' || pub_key.back() == '\r')) {
                pub_key.pop_back();
            }
            if (!pub_key.empty() && !utils::validate_ssh_public_key(pub_key)) {
                utils::get_logger()->error("Existing public key has invalid format: {}", pub_path.c_str());
                return false;
            }
        }

        utils::get_logger()->info("SSH key already exists: {}", key_path.c_str());
        return true;
    }

    auto result = utils::exec_safe({
        "ssh-keygen", "-t", key_type,
        "-f", key_path.string(), "-N", "", "-C", "ai-mirror", "-q",
        "-b", (key_type == "rsa" ? "4096" : (key_type == "ecdsa" ? "521" : ""))
    });
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
        if (!security::safe_create_directories(ssh_dir)) {
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

// Authorizes a public key for the given user's authorized_keys.
// Validates key format via validate_ssh_public_key() to reject malformed
// or malicious key files before writing to authorized_keys.
bool SSHManager::authorize_key(const std::string& username, const fs::path& public_key_path) {
    if (!ensure_ssh_dir(username)) {
        return false;
    }

    std::string home = utils::get_home_dir(username);
    fs::path auth_keys = fs::path(home) / ".ssh" / "authorized_keys";

    std::string key_content;
    if (!safe_read_public_key_file(public_key_path, key_content, getuid())) {
        return false;
    }

    if (!utils::validate_ssh_public_key(key_content)) {
        utils::get_logger()->error("Invalid SSH public key format in file: {}", public_key_path.string());
        return false;
    }

    std::vector<std::string> existing_lines;
    if (!safe_read_authorized_keys(auth_keys, existing_lines)) {
        return false;
    }

    for (const auto& l : existing_lines) {
        if (l == key_content) {
            utils::get_logger()->info("Key already authorized for {}", username);
            return true;
        }
    }

    existing_lines.push_back(key_content);

    fs::path tmp_path = make_random_tmp_path(auth_keys);
    if (!safe_write_temp_file(auth_keys, existing_lines)) {
        utils::get_logger()->error("safe_write_temp_file failed for {}", username);
        return false;
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
    if (!safe_read_authorized_keys(auth_keys, existing_lines)) {
        return false;
    }

    for (const auto& l : existing_lines) {
        if (l == public_key) {
            utils::get_logger()->info("Key already authorized for {}", username);
            return true;
        }
    }

    existing_lines.push_back(public_key);

    fs::path tmp_path = make_random_tmp_path(auth_keys);
    if (!safe_write_temp_file(auth_keys, existing_lines)) {
        utils::get_logger()->error("safe_write_temp_file failed for {}", username);
        return false;
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

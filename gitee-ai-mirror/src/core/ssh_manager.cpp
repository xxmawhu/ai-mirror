#include "ai_mirror/core/ssh_manager.hpp"
#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/logger.hpp"
#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/unique_fd.hpp"
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>

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
  utils::unique_fd ufd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (!ufd) {
    utils::get_logger()->error("Failed to open /dev/urandom");
    return "";
  }
  ssize_t n = ::read(ufd.get(), buf.data(), bytes);
  ufd.reset();
  if (n != static_cast<ssize_t>(bytes)) {
    utils::get_logger()->error("Failed to read from /dev/urandom");
    return "";
  }
  std::ostringstream oss;
  for (size_t i = 0; i < bytes; ++i) {
    oss << std::hex << std::setfill('0') << std::setw(2)
        << static_cast<int>(buf[i]);
  }
  return oss.str();
}

fs::path make_random_tmp_path(const fs::path &base) {
  std::string suffix = ".tmp." + get_crypto_random_hex(8);
  fs::path tmp = base;
  tmp += suffix;
  return tmp;
}

bool safe_write_temp_file(const fs::path &path,
                          const std::vector<std::string> &lines,
                          fs::path &out_tmp_path) {
  utils::unique_fd ufd;
  for (int attempt = 0; attempt < 3; ++attempt) {
    out_tmp_path = make_random_tmp_path(path);
    ufd.reset(::open(out_tmp_path.c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                     0600));
    if (ufd)
      break;
    if (errno != EEXIST) {
      utils::get_logger()->error("safe_write_temp_file: open({}) failed: {}",
                                 out_tmp_path.c_str(), strerror(errno));
      return false;
    }
  }
  if (!ufd) {
    utils::get_logger()->error("safe_write_temp_file: exhausted retries for {}",
                               path.c_str());
    return false;
  }

  FILE *f = ::fdopen(ufd.release(), "w");
  if (!f) {
    fs::remove(out_tmp_path);
    utils::get_logger()->error("safe_write_temp_file: fdopen failed for {}",
                               out_tmp_path.c_str());
    return false;
  }
  for (const auto &l : lines) {
    if (::fputs(l.c_str(), f) == EOF || ::fputc('\n', f) == EOF) {
      ::fclose(f);
      fs::remove(out_tmp_path);
      utils::get_logger()->error("safe_write_temp_file: write failed for {}",
                                 out_tmp_path.c_str());
      return false;
    }
  }
  if (::fflush(f) != 0) {
    ::fclose(f);
    fs::remove(out_tmp_path);
    utils::get_logger()->error("safe_write_temp_file: fflush failed for {}",
                               out_tmp_path.c_str());
    return false;
  }
  ::fclose(f);
  return true;
}

bool safe_read_authorized_keys(const fs::path &path,
                               std::vector<std::string> &lines_out) {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    utils::get_logger()->error(
        "safe_read_authorized_keys: lstat({}) failed: {}", path.c_str(),
        strerror(errno));
    return false;
  }

  uid_t expected_uid = utils::get_login_uid();
  if (expected_uid == 0)
    expected_uid = getuid();

  if (S_ISLNK(st.st_mode)) {
    // Symlink allowed: validate target ownership
    struct stat target_st;
    if (::stat(path.c_str(), &target_st) != 0) {
      utils::get_logger()->error(
          "safe_read_authorized_keys: symlink target cannot be resolved: {}",
          path.c_str());
      return false;
    }
    if (target_st.st_uid != expected_uid) {
      utils::get_logger()->error(
          "safe_read_authorized_keys: symlink target not owned by expected "
          "user (uid {} != {}): {}",
          target_st.st_uid, expected_uid, path.c_str());
      return false;
    }
    // Target validated - read the resolved path (without O_NOFOLLOW)
    utils::get_logger()->info(
        "safe_read_authorized_keys: symlink validated, reading target: {}",
        path.c_str());
    utils::unique_fd ufd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!ufd) {
      utils::get_logger()->error(
          "safe_read_authorized_keys: open({}) failed: {}", path.c_str(),
          strerror(errno));
      return false;
    }
    char buf[4096];
    std::string content;
    ssize_t n;
    while ((n = ::read(ufd.get(), buf, sizeof(buf))) > 0) {
      content.append(buf, n);
    }
    ufd.reset();
    if (n < 0) {
      utils::get_logger()->error("safe_read_authorized_keys: read failed: {}",
                                 strerror(errno));
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

  utils::unique_fd ufd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (!ufd) {
    utils::get_logger()->error("safe_read_authorized_keys: open({}) failed: {}",
                               path.c_str(), strerror(errno));
    return false;
  }

  char buf[4096];
  std::string content;
  ssize_t n;
  while ((n = ::read(ufd.get(), buf, sizeof(buf))) > 0) {
    content.append(buf, n);
  }
  ufd.reset();

  if (n < 0) {
    utils::get_logger()->error("safe_read_authorized_keys: read failed: {}",
                               strerror(errno));
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

bool safe_read_public_key_file(const fs::path &path, std::string &content_out,
                               [[maybe_unused]] uid_t expected_uid) {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) {
    utils::get_logger()->error(
        "safe_read_public_key_file: lstat({}) failed: {}", path.c_str(),
        strerror(errno));
    return false;
  }

  int open_flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;

  if (S_ISLNK(st.st_mode)) {
    // Symlink allowed: validate target ownership
    uid_t check_uid = utils::get_login_uid();
    if (check_uid == 0)
      check_uid = getuid();

    struct stat target_st;
    if (::stat(path.c_str(), &target_st) != 0) {
      utils::get_logger()->error(
          "safe_read_public_key_file: symlink target cannot be resolved: {}",
          path.c_str());
      return false;
    }
    if (target_st.st_uid != check_uid) {
      utils::get_logger()->error(
          "safe_read_public_key_file: symlink target not owned by expected "
          "user (uid {} != {}): {}",
          target_st.st_uid, check_uid, path.c_str());
      return false;
    }
    // Target validated - read without O_NOFOLLOW so the symlink is followed
    utils::get_logger()->info(
        "safe_read_public_key_file: symlink validated, reading target: {}",
        path.c_str());
    open_flags = O_RDONLY | O_CLOEXEC;
  }

  utils::unique_fd ufd(::open(path.c_str(), open_flags));
  if (!ufd) {
    utils::get_logger()->error("safe_read_public_key_file: open({}) failed: {}",
                               path.c_str(), strerror(errno));
    return false;
  }

  char buf[4096];
  std::string content;
  ssize_t n;
  while ((n = ::read(ufd.get(), buf, sizeof(buf))) > 0) {
    content.append(buf, n);
  }
  ufd.reset();

  if (n < 0) {
    utils::get_logger()->error("safe_read_public_key_file: read failed: {}",
                               strerror(errno));
    return false;
  }

  while (!content.empty() &&
         (content.back() == '\n' || content.back() == '\r')) {
    content.pop_back();
  }

  if (content.empty()) {
    utils::get_logger()->error("safe_read_public_key_file: file is empty: {}",
                               path.c_str());
    return false;
  }

  content_out = std::move(content);
  return true;
}
} // namespace

SSHManager::SSHManager() {
  key_path_ = fs::path(utils::get_effective_home()) / ".ssh" / "ai-mirror";
}

void SSHManager::set_key_path(const fs::path &path) {
  std::string p = path.string();
  if (p.size() >= 2 && p[0] == '~' && p[1] == '/') {
    key_path_ = fs::path(utils::get_effective_home()) / p.substr(2);
  } else if (p.size() >= 1 && p[0] == '~') {
    key_path_ = fs::path(utils::get_effective_home()) / p.substr(1);
  } else {
    key_path_ = path;
  }
}

// Generates or validates an existing SSH key pair.  When a key already exists
// at key_path, validates ownership (must be current user), rejects symlinks,
// warns on unsafe permissions (!=0600), and verifies the public key format via
// validate_ssh_public_key().  This prevents an attacker from pre-placing a
// malicious key or replacing the key with a symlink to a system file.
bool SSHManager::generate_key_pair(const fs::path &key_path,
                                   const std::string &key_type) {
  if (!utils::validate_key_type(key_type)) {
    utils::get_logger()->error("Invalid SSH key type: {}", key_type);
    return false;
  }

  fs::path ssh_dir = key_path.parent_path();
  std::error_code ec;
  if (!fs::exists(ssh_dir, ec)) {
    if (!security::safe_create_directories(ssh_dir)) {
      utils::get_logger()->error("Failed to create SSH directory: {}",
                                 ssh_dir.c_str());
      return false;
    }
    auto r = utils::exec_safe({"chmod", "700", ssh_dir.string()});
    if (r.exit_code != 0) {
      utils::get_logger()->error("chmod 700 failed: {}", r.stderr_output);
    }
    // chown ssh dir to login user so they can access their keys
    uid_t dir_uid = utils::get_login_uid();
    if (dir_uid != 0 && dir_uid != static_cast<uid_t>(-1)) {
      if (chown(ssh_dir.c_str(), dir_uid, static_cast<gid_t>(-1)) != 0) {
        utils::get_logger()->warn(
            "Failed to chown SSH directory to loginuid {}: {}", dir_uid,
            strerror(errno));
      }
    }
  }

  if (fs::exists(key_path, ec)) {
    struct stat st;
    if (lstat(key_path.c_str(), &st) != 0) {
      utils::get_logger()->error("Cannot stat existing key: {}",
                                 key_path.c_str());
      return false;
    }
    if (S_ISLNK(st.st_mode)) {
      utils::get_logger()->error("SSH key is a symlink, rejecting: {}",
                                 key_path.c_str());
      return false;
    }
    if ((st.st_mode & 0777) != 0600) {
      utils::get_logger()->warn(
          "SSH private key has unsafe permissions {:o}, expected 0600: {}",
          st.st_mode & 0777, key_path.c_str());
    }

    fs::path pub_path = fs::path(key_path.string() + ".pub");
    if (fs::exists(pub_path, ec)) {
      std::ifstream ifs(pub_path);
      std::string pub_key((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
      while (!pub_key.empty() &&
             (pub_key.back() == '\n' || pub_key.back() == '\r')) {
        pub_key.pop_back();
      }
      if (!pub_key.empty() && !utils::validate_ssh_public_key(pub_key)) {
        utils::get_logger()->error("Existing public key has invalid format: {}",
                                   pub_path.c_str());
        return false;
      }
    }

    utils::get_logger()->info("SSH key already exists: {}", key_path.c_str());
    return true;
  }

  std::vector<std::string> keygen_args = {
      "ssh-keygen", "-t", key_type, "-f",        key_path.string(),
      "-N",         "",   "-C",     "ai-mirror", "-q"};
  if (key_type == "rsa") {
    keygen_args.push_back("-b");
    keygen_args.push_back("4096");
  } else if (key_type == "ecdsa") {
    keygen_args.push_back("-b");
    keygen_args.push_back("521");
  }

  auto result = utils::exec_safe(keygen_args);
  if (result.exit_code != 0) {
    utils::get_logger()->error("ssh-keygen failed: {}",
                               result.stderr_output.c_str());
    return false;
  }

  uid_t login_uid = utils::get_login_uid();
  if (login_uid != 0 && login_uid != static_cast<uid_t>(-1)) {
    if (chown(key_path.c_str(), login_uid, static_cast<gid_t>(-1)) != 0) {
      utils::get_logger()->warn(
          "Failed to chown SSH private key to loginuid {}: {}", login_uid,
          strerror(errno));
    }
    fs::path pub_path = key_path;
    pub_path += ".pub";
    if (chown(pub_path.c_str(), login_uid, static_cast<gid_t>(-1)) != 0) {
      utils::get_logger()->warn(
          "Failed to chown SSH public key to loginuid {}: {}", login_uid,
          strerror(errno));
    }
  }

  utils::get_logger()->info("Generated SSH key pair: {}", key_path.c_str());
  return true;
}

bool SSHManager::ensure_ssh_dir(const std::string &username) {
  if (!utils::validate_username(username)) {
    utils::get_logger()->error("Invalid username for SSH dir: {}", username);
    return false;
  }

  std::string home = utils::get_home_dir(username);
  if (home.empty()) {
    utils::get_logger()->error("Cannot determine home for user '{}'", username);
    return false;
  }
  fs::path ssh_dir = fs::path(home) / ".ssh";

  std::error_code ec;
  bool need_chown = false;
  if (!fs::exists(ssh_dir, ec) || ec) {
    if (!security::safe_create_directories(ssh_dir)) {
      utils::get_logger()->error("Failed to create .ssh dir for {}: {}",
                                 username, ssh_dir.string());
      return false;
    }
    need_chown = true;
  }
  if (!fs::exists(ssh_dir, ec) || ec) {
    utils::get_logger()->error("Failed to verify .ssh dir exists for {}: {}",
                               username, ssh_dir.string());
    return false;
  }
  if (!fs::is_directory(ssh_dir, ec) || ec) {
    utils::get_logger()->error(".ssh path is not a directory for {}: {}",
                               username, ssh_dir.string());
    return false;
  }

  struct passwd *pw = getpwnam(username.c_str());
  if (pw) {
    struct stat st;
    if (::stat(ssh_dir.c_str(), &st) == 0) {
      if (st.st_uid != pw->pw_uid || st.st_gid != pw->pw_gid) {
        need_chown = true;
      }
    }
  }

  if (need_chown && pw) {
    std::string uid_gid =
        std::to_string(pw->pw_uid) + ":" + std::to_string(pw->pw_gid);
    auto chown_r = utils::exec_safe({"chown", uid_gid, ssh_dir.string()});
    if (chown_r.exit_code != 0) {
      utils::get_logger()->error("chown .ssh failed for {}: {}", username,
                                 chown_r.stderr_output);
      return false;
    }
    auto chmod_r = utils::exec_safe({"chmod", "700", ssh_dir.string()});
    if (chmod_r.exit_code != 0) {
      utils::get_logger()->error("chmod 700 .ssh failed for {}: {} (path: {})",
                                 username, chmod_r.stderr_output,
                                 ssh_dir.string());
      return false;
    }
  }
  return true;
}

// Authorizes a public key for the given user's authorized_keys.
// Validates key format via validate_ssh_public_key() to reject malformed
// or malicious key files before writing to authorized_keys.
bool SSHManager::authorize_key(const std::string &username,
                               const fs::path &public_key_path) {
  if (!ensure_ssh_dir(username)) {
    return false;
  }

  std::string home = utils::get_home_dir(username);
  fs::path auth_keys = fs::path(home) / ".ssh" / "authorized_keys";

  std::string key_content;
  if (!safe_read_public_key_file(public_key_path, key_content,
                                 utils::get_login_uid())) {
    return false;
  }

  if (!utils::validate_ssh_public_key(key_content)) {
    utils::get_logger()->error("Invalid SSH public key format in file: {}",
                               public_key_path.string());
    return false;
  }

  std::vector<std::string> existing_lines;
  if (!safe_read_authorized_keys(auth_keys, existing_lines)) {
    return false;
  }

  for (const auto &l : existing_lines) {
    if (l == key_content) {
      utils::get_logger()->info("Key already authorized for {}", username);
      return true;
    }
  }

  existing_lines.push_back(key_content);

  fs::path tmp_path;
  if (!safe_write_temp_file(auth_keys, existing_lines, tmp_path)) {
    utils::get_logger()->error("safe_write_temp_file failed for {}", username);
    return false;
  }

  std::error_code ec;
  fs::rename(tmp_path, auth_keys, ec);
  if (ec) {
    utils::get_logger()->error("Atomic rename failed for {}: {}", username,
                               ec.message());
    std::error_code remove_ec;
    if (!fs::remove(tmp_path, remove_ec) && remove_ec) {
      utils::get_logger()->warn("Failed to remove temp file {}: {}",
                                tmp_path.string(), remove_ec.message());
    }
    return false;
  }

  struct passwd *pw = getpwnam(username.c_str());
  if (pw) {
    std::string uid_gid =
        std::to_string(pw->pw_uid) + ":" + std::to_string(pw->pw_gid);
    auto chown_r = utils::exec_safe({"chown", uid_gid, auth_keys.string()});
    if (chown_r.exit_code != 0) {
      utils::get_logger()->error("chown authorized_keys failed for {}: {}",
                                 username, chown_r.stderr_output);
      return false;
    }
  }
  auto chmod_r = utils::exec_safe({"chmod", "600", auth_keys.string()});
  if (chmod_r.exit_code != 0) {
    utils::get_logger()->error("chmod 600 authorized_keys failed for {}",
                               username);
    return false;
  }

  utils::get_logger()->info("Authorized key for {}", username);
  return true;
}

bool SSHManager::authorize_public_key_string(const std::string &username,
                                             const std::string &public_key) {
  if (!utils::validate_ssh_public_key(public_key)) {
    utils::get_logger()->error("Invalid SSH public key format for {}",
                               username);
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

  for (const auto &l : existing_lines) {
    if (l == public_key) {
      utils::get_logger()->info("Key already authorized for {}", username);
      return true;
    }
  }

  existing_lines.push_back(public_key);

  fs::path tmp_path2;
  if (!safe_write_temp_file(auth_keys, existing_lines, tmp_path2)) {
    utils::get_logger()->error("safe_write_temp_file failed for {}", username);
    return false;
  }

  std::error_code ec2;
  fs::rename(tmp_path2, auth_keys, ec2);
  if (ec2) {
    utils::get_logger()->error("Atomic rename failed for {}: {}", username,
                               ec2.message());
    std::error_code remove_ec2;
    if (!fs::remove(tmp_path2, remove_ec2) && remove_ec2) {
      utils::get_logger()->warn("Failed to remove temp file {}: {}",
                                tmp_path2.string(), remove_ec2.message());
    }
    return false;
  }

  struct passwd *pw2 = getpwnam(username.c_str());
  if (pw2) {
    std::string uid_gid2 =
        std::to_string(pw2->pw_uid) + ":" + std::to_string(pw2->pw_gid);
    auto chown_r2 = utils::exec_safe({"chown", uid_gid2, auth_keys.string()});
    if (chown_r2.exit_code != 0) {
      utils::get_logger()->error("chown authorized_keys failed for {}: {}",
                                 username, chown_r2.stderr_output);
      return false;
    }
  }
  auto chmod_r = utils::exec_safe({"chmod", "600", auth_keys.string()});
  if (chmod_r.exit_code != 0) {
    utils::get_logger()->error("chmod 600 failed for {}", username);
    return false;
  }

  utils::get_logger()->info("Authorized inline key for {}", username);
  return true;
}

bool SSHManager::setup_default_key_from_file(const std::string &ai_user,
                                             const fs::path &key_path) {
  if (key_path.empty()) {
    utils::get_logger()->info(
        "No ai_default_key configured, skipping default key setup");
    return true;
  }

  std::error_code ec;

  // Resolve actual private and public key paths from the config key_path
  // key_path can be: a .pub file, a private key file, or a key without
  // extension
  fs::path priv_key_path, pub_key_path;

  if (key_path.extension() == ".pub") {
    // Config points to .pub → derive private key by removing .pub
    pub_key_path = key_path;
    priv_key_path =
        fs::path(key_path.string().substr(0, key_path.string().size() - 4));
  } else {
    // Config points to private key (or key without extension)
    priv_key_path = key_path;
    pub_key_path = fs::path(key_path.string() + ".pub");
  }

  // Ensure public key exists: generate from private key if needed
  if (!fs::exists(pub_key_path, ec) || ec) {
    if (fs::exists(priv_key_path, ec) && !ec) {
      auto result =
          utils::exec_safe({"ssh-keygen", "-y", "-f", priv_key_path.string()});
      if (result.exit_code != 0) {
        utils::get_logger()->warn("Failed to derive public key from {}: {}",
                                  priv_key_path.string(), result.stderr_output);
      } else {
        utils::unique_fd ufd(
            ::open(pub_key_path.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
        if (ufd) {
          FILE *f = ::fdopen(ufd.release(), "w");
          if (f) {
            fputs(result.stdout_output.c_str(), f);
            if (!result.stdout_output.empty() &&
                result.stdout_output.back() != '\n')
              fputc('\n', f);
            fclose(f);
            uid_t login_uid = utils::get_login_uid();
            if (login_uid == 0)
              login_uid = getuid();
            if (::chown(pub_key_path.c_str(), login_uid,
                        static_cast<gid_t>(-1)) != 0) {
              utils::get_logger()->warn(
                  "Failed to chown generated public key: {}",
                  pub_key_path.string());
            }
            utils::get_logger()->info(
                "Generated public key from private key: {}",
                pub_key_path.string());
          }
        }
      }
    }
  }

  // Read public key to determine key type
  if (!fs::exists(pub_key_path, ec) || ec) {
    utils::get_logger()->info("Default public key file not found, skipping: {}",
                              pub_key_path.string());
    return true;
  }

  std::ifstream pub_in(pub_key_path);
  if (!pub_in.is_open()) {
    utils::get_logger()->warn("Cannot read default public key: {}",
                              pub_key_path.string());
    return false;
  }
  std::string pub_content((std::istreambuf_iterator<char>(pub_in)),
                          std::istreambuf_iterator<char>());
  pub_in.close();

  // Determine SSH default filenames based on key type
  std::string key_basename = "id_default";     // private key filename
  std::string pub_filename = "id_default.pub"; // public key filename
  if (pub_content.find("ssh-ed25519") != std::string::npos) {
    key_basename = "id_ed25519";
    pub_filename = "id_ed25519.pub";
  } else if (pub_content.find("ssh-rsa") != std::string::npos) {
    key_basename = "id_rsa";
    pub_filename = "id_rsa.pub";
  } else if (pub_content.find("ecdsa-sha2-nistp256") != std::string::npos ||
             pub_content.find("ecdsa-sha2-nistp384") != std::string::npos ||
             pub_content.find("ecdsa-sha2-nistp521") != std::string::npos) {
    key_basename = "id_ecdsa";
    pub_filename = "id_ecdsa.pub";
  }

  // Resolve ai-user
  struct passwd *pw = getpwnam(ai_user.c_str());
  if (!pw) {
    utils::get_logger()->warn(
        "Cannot resolve ai-user '{}' for default key setup", ai_user);
    return false;
  }

  fs::path ai_ssh_dir = fs::path(pw->pw_dir) / ".ssh";
  if (!fs::exists(ai_ssh_dir, ec) || ec) {
    utils::get_logger()->warn("ai-user .ssh directory does not exist: {}",
                              ai_ssh_dir.string());
    return false;
  }

  // --- Copy PUBLIC key to ai-user ~/.ssh/<pub_filename> ---
  fs::path ai_target_pub = ai_ssh_dir / pub_filename;
  bool pub_unchanged = false;
  if (fs::exists(ai_target_pub, ec) && !ec) {
    std::ifstream existing(ai_target_pub);
    if (existing.is_open()) {
      std::string existing_content((std::istreambuf_iterator<char>(existing)),
                                   std::istreambuf_iterator<char>());
      existing.close();
      if (existing_content == pub_content)
        pub_unchanged = true;
    }
  }

  if (!pub_unchanged) {
    int fd =
        ::open(ai_target_pub.c_str(),
               O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
      utils::get_logger()->warn(
          "Failed to create public key in ai-user .ssh: {}",
          ai_target_pub.string());
      return false;
    }
    ssize_t written = ::write(fd, pub_content.data(), pub_content.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(pub_content.size())) {
      utils::get_logger()->warn("Failed to write public key to {}",
                                ai_target_pub.string());
      return false;
    }
    if (::chown(ai_target_pub.c_str(), pw->pw_uid, pw->pw_gid) != 0) {
      utils::get_logger()->warn("Failed to chown public key: {}",
                                ai_target_pub.string());
    }
    utils::get_logger()->info("Installed default public key: {} → {}",
                              pub_key_path.string(), ai_target_pub.string());
  }

  // --- Copy PRIVATE key to ai-user ~/.ssh/<key_basename> ---
  if (fs::exists(priv_key_path, ec) && !ec) {
    fs::path ai_target_priv = ai_ssh_dir / key_basename;

    // Read private key content
    std::ifstream priv_in(priv_key_path);
    if (!priv_in.is_open()) {
      utils::get_logger()->warn("Cannot read private key: {}",
                                priv_key_path.string());
    } else {
      std::string priv_content((std::istreambuf_iterator<char>(priv_in)),
                               std::istreambuf_iterator<char>());
      priv_in.close();

      // Check if already identical
      bool priv_unchanged = false;
      if (fs::exists(ai_target_priv, ec) && !ec) {
        std::ifstream existing(ai_target_priv);
        if (existing.is_open()) {
          std::string existing_content(
              (std::istreambuf_iterator<char>(existing)),
              std::istreambuf_iterator<char>());
          existing.close();
          if (existing_content == priv_content)
            priv_unchanged = true;
        }
      }

      if (!priv_unchanged) {
        // Write private key with restrictive permissions (0600)
        int fd =
            ::open(ai_target_priv.c_str(),
                   O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
          utils::get_logger()->warn(
              "Failed to create private key in ai-user .ssh: {}",
              ai_target_priv.string());
        } else {
          ssize_t written =
              ::write(fd, priv_content.data(), priv_content.size());
          ::close(fd);
          if (written != static_cast<ssize_t>(priv_content.size())) {
            utils::get_logger()->warn("Failed to write private key to {}",
                                      ai_target_priv.string());
          } else {
            if (::chown(ai_target_priv.c_str(), pw->pw_uid, pw->pw_gid) != 0) {
              utils::get_logger()->warn("Failed to chown private key: {}",
                                        ai_target_priv.string());
            }
            utils::get_logger()->info("Installed default private key: {} → {}",
                                      priv_key_path.string(),
                                      ai_target_priv.string());
          }
        }
      }
    }
  } else {
    utils::get_logger()->info(
        "No private key found at {}, only public key installed",
        priv_key_path.string());
  }

  utils::get_logger()->info("Default key setup complete for ai-user {} "
                            "(identity key, not authorized_keys)",
                            ai_user);
  return true;
}

KeySetupResult
SSHManager::setup_default_keys(const std::string &ai_user,
                               const std::vector<SSHKeyEntry> &default_keys) {
  KeySetupResult result;
  result.total_count = default_keys.size();
  result.authorized_count = 0;

  for (const auto &entry : default_keys) {
    if (entry.public_key.empty())
      continue;
    if (authorize_public_key_string(ai_user, entry.public_key)) {
      ++result.authorized_count;
    } else {
      std::string name =
          entry.name.empty() ? std::string("(unnamed)") : entry.name;
      utils::get_logger()->warn(
          std::string("Failed to authorize default key '") + name + "' for " +
          ai_user);
    }
  }

  result.success = (result.authorized_count == result.total_count);
  return result;
}

bool SSHManager::setup_passwordless(const std::string &main_user,
                                    const std::string &ai_user) {
  fs::path pub_key = get_public_key_path(key_path_);

  if (!generate_key_pair(key_path_, key_type_)) {
    return false;
  }

  if (!authorize_key(ai_user, pub_key)) {
    return false;
  }

  utils::get_logger()->info("Passwordless SSH setup: {} -> {}",
                            main_user.c_str(), ai_user.c_str());
  return true;
}

fs::path SSHManager::get_public_key_path(const fs::path &key_path) const {
  return fs::path(key_path.string() + ".pub");
}

bool SSHManager::test_connection(const std::string &username) const {
  if (!utils::validate_username(username))
    return false;

  auto result = utils::exec_safe({"ssh", "-o", "BatchMode=yes", "-o",
                                  "ConnectTimeout=5", "-i", key_path_.string(),
                                  username + "@localhost", "echo ok"});
  return result.exit_code == 0;
}

// Sync known_hosts from main_user to ai_user by file copy.
// Copies main_user's known_hosts to ai_user's .ssh/ directory.
// Handles hashed entries (| prefix) that ssh-keyscan cannot process.
bool SSHManager::sync_known_hosts(const std::string &main_user,
                                  const std::string &ai_user) {
  if (!utils::validate_username(main_user) ||
      !utils::validate_username(ai_user)) {
    utils::get_logger()->error("Invalid username for sync_known_hosts");
    return false;
  }

  // Get main_user's known_hosts path
  struct passwd *main_pw = getpwnam(main_user.c_str());
  if (!main_pw) {
    utils::get_logger()->error("Cannot resolve main user '{}'", main_user);
    return false;
  }
  fs::path main_known_hosts =
      fs::path(main_pw->pw_dir) / ".ssh" / "known_hosts";

  // Check if main_user's known_hosts exists
  std::error_code ec;
  if (!fs::exists(main_known_hosts, ec)) {
    utils::get_logger()->info(
        "Main user {} has no known_hosts file, skipping sync", main_user);
    return true; // Not an error, just nothing to sync
  }

  // Get ai_user info
  struct passwd *ai_pw = getpwnam(ai_user.c_str());
  if (!ai_pw) {
    utils::get_logger()->error("Cannot resolve ai user '{}'", ai_user);
    return false;
  }
  fs::path ai_ssh_dir = fs::path(ai_pw->pw_dir) / ".ssh";
  fs::path ai_known_hosts = ai_ssh_dir / "known_hosts";

  // Ensure ai_user's .ssh directory exists
  std::error_code dir_ec;
  if (!fs::exists(ai_ssh_dir, dir_ec)) {
    fs::create_directories(ai_ssh_dir, dir_ec);
    if (dir_ec) {
      utils::get_logger()->error("Cannot create .ssh dir for {}: {}", ai_user,
                                 dir_ec.message());
      return false;
    }
  }

  // Copy known_hosts file (overwrite if exists)
  std::error_code copy_ec;
  fs::copy_file(main_known_hosts, ai_known_hosts,
                fs::copy_options::overwrite_existing, copy_ec);
  if (copy_ec) {
    utils::get_logger()->error("Failed to copy known_hosts from {} to {}: {}",
                               main_known_hosts.string(),
                               ai_known_hosts.string(), copy_ec.message());
    return false;
  }

  // Set ownership to ai_user (critical: without this ai_user cannot read the
  // file)
  auto chown_result = utils::exec_safe(
      {"chown", ai_user + ":" + ai_user, ai_known_hosts.string()});
  if (chown_result.exit_code != 0) {
    utils::get_logger()->error("Failed to chown known_hosts for {}", ai_user);
    return false;
  }

  // Set permissions (600: owner read/write only, no group/other access)
  auto chmod_result =
      utils::exec_safe({"chmod", "600", ai_known_hosts.string()});
  if (chmod_result.exit_code != 0) {
    utils::get_logger()->error("Failed to chmod known_hosts for {}", ai_user);
    return false;
  }

  // Ensure .ssh directory ownership and permissions are correct
  // (covers both newly created and pre-existing directories)
  auto dir_chown =
      utils::exec_safe({"chown", ai_user + ":" + ai_user, ai_ssh_dir.string()});
  if (dir_chown.exit_code != 0) {
    utils::get_logger()->error("Failed to chown .ssh dir for {}", ai_user);
    return false;
  }
  auto dir_chmod = utils::exec_safe({"chmod", "700", ai_ssh_dir.string()});
  if (dir_chmod.exit_code != 0) {
    utils::get_logger()->error("Failed to chmod .ssh dir for {}", ai_user);
    return false;
  }

  utils::get_logger()->info(
      "Synced known_hosts from {} to {} (file copy, includes hashed entries)",
      main_user, ai_user);
  return true;
}

} // namespace ai_mirror::core

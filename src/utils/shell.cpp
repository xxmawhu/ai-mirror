#include "ai_mirror/utils/shell.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <array>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <set>
#include <map>
#include <vector>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/types.h>

namespace ai_mirror::utils {

static constexpr size_t PIPE_BUF_SIZE = 4096;
// Maximum bytes to read from a subprocess pipe (10MB).  Prevents memory
// exhaustion if a malicious or buggy subprocess produces infinite output
// (e.g. `while true; do echo x; done`).  Once exceeded, the read is truncated
// and a warning logged; the caller receives partial output rather than OOM.
static constexpr size_t MAX_READ_SIZE = 10 * 1024 * 1024;

static std::string read_fd(int fd) {
    std::string result;
    result.reserve(PIPE_BUF_SIZE);
    std::array<char, PIPE_BUF_SIZE> buf{};
    ssize_t n;
    while ((n = ::read(fd, buf.data(), buf.size() - 1)) > 0) {
        buf.data()[n] = '\0';
        result += buf.data();
        if (result.size() > MAX_READ_SIZE) {
            get_logger()->warn("read_fd: output exceeded {} bytes, truncating", MAX_READ_SIZE);
            break;
        }
    }
    return result;
}

static std::string resolve_command(const std::string& cmd) {
    if (cmd.empty()) return "";

    static const std::map<std::string, std::string> COMMAND_PATHS = {
        {"mount", "/usr/bin/mount"},
        {"umount", "/usr/bin/umount"},
        {"chmod", "/usr/bin/chmod"},
        {"chown", "/usr/bin/chown"},
        {"chgrp", "/usr/bin/chgrp"},
        {"useradd", "/usr/sbin/useradd"},
        {"userdel", "/usr/sbin/userdel"},
        {"groupadd", "/usr/sbin/groupadd"},
        {"groupdel", "/usr/sbin/groupdel"},
        {"usermod", "/usr/sbin/usermod"},
        {"passwd", "/usr/bin/passwd"},
        {"gpasswd", "/usr/bin/gpasswd"},
        {"ssh-keygen", "/usr/bin/ssh-keygen"},
        {"mkdir", "/usr/bin/mkdir"},
        {"cp", "/usr/bin/cp"},
        {"mv", "/usr/bin/mv"},
        {"getent", "/usr/bin/getent"},
        {"findmnt", "/usr/bin/findmnt"},
        {"which", "/usr/bin/which"},
        {"ssh", "/usr/bin/ssh"},
    };

    if (cmd.find('/') != std::string::npos) {
        return cmd;
    }

    auto it = COMMAND_PATHS.find(cmd);
    if (it != COMMAND_PATHS.end()) {
        return it->second;
    }

    return "";
}

static ShellResult do_fork_exec(const std::string& file, char* const argv[]) {
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (::pipe(stdout_pipe) != 0) {
        return {-1, "", "pipe() failed for stdout"};
    }
    if (::pipe(stderr_pipe) != 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return {-1, "", "pipe() failed for stderr"};
    }

    std::string resolved = resolve_command(file);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return {-1, "", "fork() failed"};
    }

    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);

        ::signal(SIGPIPE, SIG_DFL);
        ::execv(resolved.c_str(), argv);
        ::_exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    std::string stdout_out = read_fd(stdout_pipe[0]);
    std::string stderr_out = read_fd(stderr_pipe[0]);

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {exit_code, stdout_out, stderr_out};
}

ShellResult exec_safe(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {-1, "", "empty argument list"};
    }
    return exec_safe(args[0], args);
}

ShellResult exec_safe(const std::string& file, const std::vector<std::string>& args) {
    if (file.empty()) {
        return {-1, "", "empty command file"};
    }

    static const std::set<std::string> ALLOWED_COMMANDS = {
        "mount", "umount", "chmod", "chown", "chgrp",
        "useradd", "userdel", "groupadd", "groupdel", "usermod", "passwd",
        "gpasswd", "ssh-keygen", "mkdir", "cp", "mv",
        "getent", "findmnt", "which", "ssh", "pkill"
    };
    std::string cmd_name = fs::path(file).filename().string();
    if (ALLOWED_COMMANDS.find(cmd_name) == ALLOWED_COMMANDS.end()) {
        return {-1, "", "command not in allowed list: " + cmd_name};
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    return do_fork_exec(file, argv.data());
}

bool validate_username(const std::string& username) {
    if (username.empty() || username.size() > 32) return false;
    if (username[0] >= '0' && username[0] <= '9') return false;
    for (char c : username) {
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
    }
    return true;
}

bool validate_ssh_public_key(const std::string& key) {
    if (key.empty() || key.size() > 8192) return false;
    if (key.find('\'') != std::string::npos) return false;
    if (key.find('\n') != std::string::npos) return false;
    if (key.find('\r') != std::string::npos) return false;

    static const std::vector<std::string> valid_prefixes = {
        "ssh-ed25519 ", "ssh-rsa ",
        "ecdsa-sha2-nistp256 ", "ecdsa-sha2-nistp384 ", "ecdsa-sha2-nistp521 ",
        "sk-ssh-ed25519@openssh.com ", "sk-ecdsa-sha2-nistp256@openssh.com "
    };
    bool has_valid_prefix = false;
    for (const auto& p : valid_prefixes) {
        if (key.size() > p.size() && key.compare(0, p.size(), p) == 0) {
            has_valid_prefix = true;
            break;
        }
    }
    if (!has_valid_prefix) return false;

    std::regex re("^[a-zA-Z0-9+/=@._-]+(\\s+.+)?$");
    return std::regex_match(key, re);
}

bool validate_key_type(const std::string& key_type) {
    return key_type == "ed25519" || key_type == "rsa" || key_type == "ecdsa";
}

bool is_root() {
    return geteuid() == 0;
}

bool command_exists(const std::string& cmd) {
    auto result = exec_safe("which", {"which", cmd});
    return result.exit_code == 0;
}

std::string get_current_username() {
    auto* pw = getpwuid(geteuid());
    return pw ? pw->pw_name : "";
}

std::string get_home_dir(const std::string& username) {
    auto* pw = getpwnam(username.c_str());
    return pw ? pw->pw_dir : "";
}

std::string get_effective_username() {
    uid_t login_uid = get_login_uid();
    if (login_uid != 0) {
        auto* pw = getpwuid(login_uid);
        if (pw && pw->pw_name) return pw->pw_name;
    }
    return get_current_username();
}

std::string get_effective_home() {
    if (const char* env_home = std::getenv("HOME")) {
        if (env_home[0] == '/' && env_home[1] != '\0') return env_home;
    }
    std::string username = get_effective_username();
    if (!username.empty()) {
        std::string home = get_home_dir(username);
        if (!home.empty()) return home;
    }
    return get_home_dir(get_current_username());
}

bool validate_path_no_shell_metachars(const std::string& path) {
    if (path.find('\0') != std::string::npos) {
        return false;
    }
    static const std::string dangerous = ";`$(){}[]|&<>!\n\r";
    return path.find_first_of(dangerous) == std::string::npos;
}

std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

uid_t get_login_uid() {
    const char* sudo_uid_env = std::getenv("SUDO_UID");
    if (sudo_uid_env && sudo_uid_env[0] != '\0') {
        try {
            uid_t uid = std::stoul(sudo_uid_env);
            if (uid != 0) return uid;
        } catch (...) {}
    }

    std::ifstream ifs("/proc/self/loginuid");
    if (ifs) {
        uid_t uid = 0;
        ifs >> uid;
        if (uid != (uid_t)-1 && uid != 0) {
            return uid;
        }
    }
    return 0;
}

bool is_path_allowed(const fs::path& p, const std::string& main_user) {
    if (p.empty()) return false;

    for (const auto& part : p) {
        if (part == "..") return false;
    }

    std::string main_home = get_effective_home();
    if (main_home.empty()) {
        main_home = get_home_dir(main_user);
    }
    if (main_home.empty()) return false;

    std::error_code ec;
    fs::path canon = fs::canonical(p, ec);
    if (ec) {
        fs::path parent = p.parent_path();
        fs::path canon_parent = fs::canonical(parent, ec);
        if (ec) return false;
        std::string parent_str = canon_parent.string();
        if (parent_str != main_home
            && !(parent_str.length() > main_home.length()
                 && parent_str[main_home.length()] == '/'
                 && parent_str.substr(0, main_home.length()) == main_home)) {
            return false;
        }
        canon = fs::weakly_canonical(p, ec);
        if (ec) return false;
        for (const auto& part : canon) {
            if (part == "..") return false;
        }
    }

    std::string s = canon.string();

    if (s == main_home) return true;
    if (s.length() > main_home.length() && s[main_home.length()] == '/'
        && s.substr(0, main_home.length()) == main_home) return true;

    return false;
}

bool is_path_allowed_parent(const fs::path& p, const std::string& main_user) {
    fs::path parent = p.parent_path();
    if (parent.empty()) return false;
    return is_path_allowed(parent, main_user);
}

bool is_group_member(const std::string& group_name) {
    if (group_name.empty()) return false;

    // Get the group entry
    struct group* gr = getgrnam(group_name.c_str());
    if (!gr) {
        // Group doesn't exist
        return false;
    }
    gid_t target_gid = gr->gr_gid;

    // Check primary group from passwd entry
    uid_t uid = geteuid();
    struct passwd* pw = getpwuid(uid);
    if (pw && pw->pw_gid == target_gid) {
        return true;
    }

    // Check supplementary groups
    int ngroups = getgroups(0, nullptr);
    if (ngroups <= 0) {
        return false;
    }

    std::vector<gid_t> groups(ngroups);
    if (getgroups(ngroups, groups.data()) < 0) {
        return false;
    }

    for (gid_t g : groups) {
        if (g == target_gid) {
            return true;
        }
    }

    return false;
}

}

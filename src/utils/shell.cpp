#include "ai_mirror/utils/shell.hpp"
#include <array>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/types.h>

namespace ai_mirror::utils {

static constexpr size_t PIPE_BUF_SIZE = 4096;

static std::string read_fd(int fd) {
    std::string result;
    std::array<char, PIPE_BUF_SIZE> buf{};
    ssize_t n;
    while ((n = ::read(fd, buf.data(), buf.size() - 1)) > 0) {
        buf.data()[n] = '\0';
        result += buf.data();
    }
    return result;
}

static std::string resolve_command(const std::string& cmd) {
    if (cmd.empty()) return "";
    if (cmd.find('/') != std::string::npos) return cmd;

    std::string path_env;
    if (const char* p = std::getenv("PATH")) {
        path_env = p;
    }
    if (path_env.empty()) {
        path_env = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }

    std::istringstream iss(path_env);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        if (dir.empty()) continue;
        fs::path candidate = fs::path(dir) / cmd;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate.string();
        }
    }
    return cmd;
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
    std::regex re("^[a-z_][a-z0-9_-]*$");
    return std::regex_match(username, re);
}

bool validate_ssh_public_key(const std::string& key) {
    if (key.empty() || key.size() > 8192) return false;
    if (key.find('\'') != std::string::npos) return false;
    if (key.find('\n') != std::string::npos) return false;
    if (key.find('\r') != std::string::npos) return false;
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

    if (geteuid() == 0) {
        if (const char* sudo_user = std::getenv("SUDO_USER")) {
            if (sudo_user[0] != '\0' && validate_username(sudo_user)) {
                if (const char* sudo_uid_str = std::getenv("SUDO_UID")) {
                    try {
                        auto sudo_uid = static_cast<uid_t>(std::stoul(sudo_uid_str));
                        auto* pw = getpwuid(sudo_uid);
                        if (pw && pw->pw_name && pw->pw_name == std::string(sudo_user)) {
                            return sudo_user;
                        }
                    } catch (...) {}
                }
            }
        }
    }
    return get_current_username();
}

std::string get_effective_home() {
    std::string username = get_effective_username();
    if (!username.empty()) {
        std::string home = get_home_dir(username);
        if (!home.empty()) return home;
    }
    if (const char* env_home = std::getenv("HOME")) {
        if (env_home[0] != '\0') return env_home;
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

    std::string main_home = get_home_dir(main_user);
    if (main_home.empty()) return false;

    std::error_code ec;
    fs::path canon = fs::canonical(p, ec);
    if (ec) {
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

}

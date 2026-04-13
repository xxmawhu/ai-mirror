#include "ai_mirror/utils/shell.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <pwd.h>

namespace ai_mirror::utils {

ShellResult execute(const std::string& cmd) {
    std::array<char, 4096> buffer{};
    std::string stdout_out;
    std::string stderr_out;

    std::string full_cmd = cmd + " 2>/tmp/ai-mirror-stderr-tmp";
    auto pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        return {-1, "", "Failed to execute command"};
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        stdout_out += buffer.data();
    }
    int exit_code = pclose(pipe);

    {
        std::ifstream err_file("/tmp/ai-mirror-stderr-tmp");
        if (err_file.is_open()) {
            std::stringstream ss;
            ss << err_file.rdbuf();
            stderr_out = ss.str();
        }
    }

    return {exit_code, stdout_out, stderr_out};
}

ShellResult execute(const std::vector<std::string>& args) {
    std::string cmd;
    for (const auto& arg : args) {
        if (!cmd.empty()) cmd += " ";
        bool needs_quote = arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos;
        if (needs_quote) {
            cmd += "'" + arg + "'";
        } else {
            cmd += arg;
        }
    }
    return execute(cmd);
}

bool is_root() {
    return geteuid() == 0;
}

bool command_exists(const std::string& cmd) {
    auto result = execute("which " + cmd + " 2>/dev/null");
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
    if (const char* sudo_user = std::getenv("SUDO_USER")) {
        if (sudo_user[0] != '\0') return sudo_user;
    }
    if (const char* pkexec_uid = std::getenv("PKEXEC_UID")) {
        auto* pw = getpwuid(static_cast<uid_t>(std::stoul(pkexec_uid)));
        if (pw) return pw->pw_name;
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

}

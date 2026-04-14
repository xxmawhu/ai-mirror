#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <sys/types.h>

namespace fs = std::filesystem;

namespace ai_mirror::utils {

struct ShellResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
};

ShellResult exec_safe(const std::vector<std::string>& args);
ShellResult exec_safe(const std::string& file, const std::vector<std::string>& args);

bool validate_username(const std::string& username);
bool validate_ssh_public_key(const std::string& key);
bool validate_key_type(const std::string& key_type);

bool is_root();
bool command_exists(const std::string& cmd);
std::string get_current_username();
std::string get_home_dir(const std::string& username);
std::string get_effective_username();
std::string get_effective_home();
    std::string shell_escape(const std::string& s);
    bool validate_path_no_shell_metachars(const std::string& path);
    bool is_path_allowed(const fs::path& p, const std::string& main_user);
uid_t get_login_uid();

}

#pragma once

#include <string>
#include <vector>

namespace ai_mirror::utils {

struct ShellResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
};

ShellResult execute(const std::string& cmd);
ShellResult execute(const std::vector<std::string>& args);

bool is_root();
bool command_exists(const std::string& cmd);
std::string get_current_username();
std::string get_home_dir(const std::string& username);
std::string get_effective_username();
std::string get_effective_home();

}

#pragma once

#include <string>

// CLI command handlers for ai-mirror.
//
// cmd_rm() performs user deletion with process cleanup:
// 1. Unmount bind mounts via MountCleaner
// 2. Terminate active user processes via pkill -u
// 3. Remove Linux user via userdel
// 4. Delete home directory
// 5. Revoke write grants on project path
//
// All commands validate paths and usernames before execution.

namespace ai_mirror::cli {

int cmd_create(const std::string& project_path, bool verbose);
int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose);
int cmd_cp(const std::string& src, const std::string& dst, bool verbose);
int cmd_mv(const std::string& src, const std::string& dst, bool verbose);
int cmd_touch(const std::string& path, const std::string& ai_user, bool verbose);
int cmd_cd(const std::string& path, bool verbose);
int cmd_list(bool verbose);
int cmd_health(bool verbose);
int cmd_force_destroy(const std::string& project_or_user, bool verbose);
int cmd_rm(const std::string& project_path, bool verbose);
int cmd_config(bool verbose);
int cmd_status(bool verbose);

}

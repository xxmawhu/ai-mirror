#pragma once

#include <string>

namespace ai_mirror::cli {

int cmd_create(const std::string& project_path, bool verbose);
int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose);
int cmd_cd(const std::string& path, bool verbose);
int cmd_list(bool verbose);
int cmd_health(bool verbose);
int cmd_force_destroy(const std::string& project_or_user, bool verbose);
int cmd_rm(const std::string& project_path, bool verbose);
int cmd_config(bool verbose);

}

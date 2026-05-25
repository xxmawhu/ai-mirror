#pragma once

#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace ai_mirror::utils {

void init_logger(const std::string &level = "info",
                 const std::string &log_file = "");
std::shared_ptr<spdlog::logger> get_logger();

} // namespace ai_mirror::utils

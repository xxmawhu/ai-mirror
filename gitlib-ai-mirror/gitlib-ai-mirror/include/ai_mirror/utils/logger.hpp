#pragma once

#include <spdlog/spdlog.h>
#include <memory>
#include <string>

namespace ai_mirror::utils {

void init_logger(const std::string& level = "info", const std::string& log_file = "");
std::shared_ptr<spdlog::logger> get_logger();

}

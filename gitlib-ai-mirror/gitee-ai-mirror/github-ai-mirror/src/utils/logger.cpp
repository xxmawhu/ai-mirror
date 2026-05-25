#include "ai_mirror/utils/logger.hpp"
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace ai_mirror::utils {

static std::shared_ptr<spdlog::logger> g_logger;
static std::once_flag g_logger_once;

void init_logger(const std::string &level, const std::string &log_file) {
  std::vector<spdlog::sink_ptr> sinks;
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

  if (!log_file.empty()) {
    sinks.push_back(
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, false));
  }

  g_logger =
      std::make_shared<spdlog::logger>("ai-mirror", sinks.begin(), sinks.end());

  if (level == "debug") {
    g_logger->set_level(spdlog::level::debug);
  } else if (level == "trace") {
    g_logger->set_level(spdlog::level::trace);
  } else if (level == "warn" || level == "warning") {
    g_logger->set_level(spdlog::level::warn);
  } else if (level == "error") {
    g_logger->set_level(spdlog::level::err);
  } else {
    g_logger->set_level(spdlog::level::info);
  }

  g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> get_logger() {
  std::call_once(g_logger_once, [] {
    if (!g_logger) {
      init_logger();
    }
  });
  return g_logger;
}

} // namespace ai_mirror::utils

#include "ai_mirror/utils/logger.hpp"
#include <memory>
#include <mutex>
// spdlog 1.15.x removed stderr_color_sink.h; use ansicolor_sink.h instead
#include <spdlog/sinks/ansicolor_sink.h>
#include <string>
#include <vector>

namespace ai_mirror::utils {

static std::shared_ptr<spdlog::logger> g_logger;
static std::once_flag g_logger_once;

void init_logger(const std::string &level) {
  // Use stderr as the sole logging sink.
  // Rationale: CLI commands emit machine-readable output (e.g. JSON from
  // `am cd --dry-run`) on stdout. Logging to stdout corrupts that output and
  // breaks the shell integration's JSON parser (root cause of the
  // "[fail] unknown action: error" bug). All diagnostic/info logging MUST go
  // to stderr so stdout stays clean for data.
  auto sink = std::make_shared<spdlog::sinks::ansicolor_stderr_sink_mt>();

  g_logger = std::make_shared<spdlog::logger>("ai-mirror", sink);

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

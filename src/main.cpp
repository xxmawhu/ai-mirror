#include "ai_mirror/cli/parser.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <chrono>
#include <exception>
#include <filesystem>
#include <sys/stat.h>

// Helper: get log file path ~/.cache/am/run.{YYYYMMDD}.log
static std::string get_log_file_path() {
  // Get HOME environment variable
  const char *home = getenv("HOME");
  if (!home) {
    return ""; // No HOME, skip file logging
  }

  std::filesystem::path cache_dir =
      std::filesystem::path(home) / ".cache" / "am";

  // Create ~/.cache/am if not exists
  std::error_code ec;
  if (!std::filesystem::exists(cache_dir, ec)) {
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
      // Failed to create, skip file logging
      return "";
    }
    // Set permissions: 0755 (owner rwx, group rx, others rx)
    chmod(cache_dir.c_str(), 0755);
  }

  // Get current date YYYYMMDD
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf;
  localtime_r(&now_time_t, &tm_buf);

  char date_buf[16];
  std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_buf);

  std::filesystem::path log_file =
      cache_dir / ("run." + std::string(date_buf) + ".log");
  return log_file.string();
}

// Exception handling strategy:
// - std::exception: logged with what() message, exit 1
// - unknown exception: use current_exception() + rethrow to extract
// type/message
//   (handles both std::exception subclasses and non-std exceptions)
// - this pattern ensures no exception is silently swallowed
int main(int argc, char **argv) {
  ai_mirror::utils::init_logger("info", get_log_file_path());
  try {
    return ai_mirror::cli::parse_and_run(argc, argv);
  } catch (const std::exception &e) {
    ai_mirror::utils::get_logger()->error("Unhandled exception: {}", e.what());
    return 1;
  } catch (...) {
    auto eptr = std::current_exception();
    try {
      if (eptr)
        std::rethrow_exception(eptr);
    } catch (const std::exception &e) {
      ai_mirror::utils::get_logger()->error(
          "Unknown exception (re-caught as std::exception): {}", e.what());
    } catch (...) {
      ai_mirror::utils::get_logger()->error(
          "Unknown non-std exception occurred");
    }
    return 1;
  }
}

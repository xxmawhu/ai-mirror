#include "ai_mirror/cli/parser.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <chrono>
#include <exception>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

// Get the real (non-root) user's UID via SUDO_UID or loginuid.
// Returns 0 if running as root directly (no sudo).
static uid_t get_real_uid() {
  const char *sudo_uid = getenv("SUDO_UID");
  if (sudo_uid && sudo_uid[0] != '\0') {
    try {
      uid_t uid = static_cast<uid_t>(std::stoul(sudo_uid));
      if (uid != 0)
        return uid;
    } catch (...) {
    }
  }
  return 0; // Not under sudo, or already root
}

// Ensure a path is owned by the real user (not root).
// Only chowns when running under sudo (get_real_uid() != 0).
static void ensure_user_ownership(const std::filesystem::path &p) {
  uid_t real_uid = get_real_uid();
  if (real_uid == 0)
    return; // Not under sudo, no need to chown

  struct stat st;
  if (stat(p.c_str(), &st) == 0 && st.st_uid != real_uid) {
    // chown to real user, keep group
    if (chown(p.c_str(), real_uid, st.st_gid) != 0) {
      // Non-fatal: best effort, log init hasn't completed yet
    }
  }
}

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
    // Ensure ownership belongs to the real user, not root
    ensure_user_ownership(cache_dir);
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

  // If log file already exists but owned by root, fix ownership
  if (std::filesystem::exists(log_file, ec)) {
    ensure_user_ownership(log_file);
  }

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

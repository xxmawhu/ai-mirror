#include "ai_mirror/cli/parser.hpp"
#include "ai_mirror/utils/logger.hpp"
#include <exception>

// Exception handling strategy:
// - std::exception: logged with what() message, exit 1
// - unknown exception: use current_exception() + rethrow to extract
// type/message
//   (handles both std::exception subclasses and non-std exceptions)
// - this pattern ensures no exception is silently swallowed
int main(int argc, char **argv) {
  ai_mirror::utils::init_logger("info");
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

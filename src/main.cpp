#include "ai_mirror/cli/parser.hpp"
#include "ai_mirror/utils/logger.hpp"

int main(int argc, char** argv) {
    ai_mirror::utils::init_logger();
    try {
        return ai_mirror::cli::parse_and_run(argc, argv);
    } catch (const std::exception& e) {
        ai_mirror::utils::get_logger()->error("Unhandled exception: {}", e.what());
        return 1;
    } catch (...) {
        ai_mirror::utils::get_logger()->error("Unknown exception occurred");
        return 1;
    }
}

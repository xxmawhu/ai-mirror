#include "ai_mirror/cli/parser.hpp"
#include "ai_mirror/utils/logger.hpp"

int main(int argc, char** argv) {
    ai_mirror::utils::init_logger();
    return ai_mirror::cli::parse_and_run(argc, argv);
}

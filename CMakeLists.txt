# cmake 3.16 is the minimum required by the most demanding vendored dep
# (toml11). Lowering from 3.28 to 3.16 broadens compatibility (Ubuntu 20.04
# ships cmake 3.16) without losing any feature used by this project.
cmake_minimum_required(VERSION 3.16)
project(ai-mirror VERSION 0.1.0 LANGUAGES CXX)

# Get git short hash for BUILD version
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE AI_MIRROR_BUILD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(AI_MIRROR_BUILD "unknown")
endif()

# Configure version header
configure_file(
    ${CMAKE_SOURCE_DIR}/include/ai_mirror/version.hpp.in
    ${CMAKE_SOURCE_DIR}/include/ai_mirror/version.hpp
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
set(CMAKE_LINK_SEARCH_START_STATIC ON)
set(CMAKE_LINK_SEARCH_END_STATIC ON)

set(AI_MIRROR_SOURCES
    src/main.cpp
    src/cli/parser.cpp
    src/cli/commands.cpp
    src/core/config.cpp
    src/core/user_manager.cpp
    src/core/graft.cpp
    src/core/ssh_manager.cpp
    src/core/path_resolver.cpp
    src/security/path_validator.cpp
    src/security/audit.cpp
    src/daemon/health_check.cpp
    src/daemon/auth_monitor.cpp
    src/daemon/mount_cleaner.cpp
    src/daemon/watch_stats.cpp
    src/utils/shell.cpp
    src/utils/logger.cpp
)

# Main implementation binary (ai-mirror-bin)
add_executable(ai-mirror-bin ${AI_MIRROR_SOURCES})

target_include_directories(ai-mirror-bin PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(ai-mirror-bin PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -Wformat -Wformat-security
    -fvisibility=hidden
    -ffunction-sections -fdata-sections
)

target_link_options(ai-mirror-bin PRIVATE
    -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
    -static-libstdc++ -static-libgcc
    -Wl,--gc-sections -Wl,--strip-all
)

# Wrapper binary (am) - auto sudo elevation
add_executable(am src/am_wrapper.cpp)

target_compile_options(am PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
)

target_link_options(am PRIVATE
    -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
    -static-libstdc++ -static-libgcc
    -Wl,--strip-all
)

# Use vendored dependencies from third_party/ (no network downloads)
add_subdirectory(third_party/CLI11)
target_link_libraries(ai-mirror-bin PRIVATE CLI11::CLI11)

add_subdirectory(third_party/toml11)
target_link_libraries(ai-mirror-bin PRIVATE toml11::toml11)

add_subdirectory(third_party/spdlog)
target_link_libraries(ai-mirror-bin PRIVATE spdlog::spdlog)

add_subdirectory(third_party/nlohmann_json)
target_link_libraries(ai-mirror-bin PRIVATE nlohmann_json::nlohmann_json)

add_subdirectory(third_party/ftxui)
target_link_libraries(ai-mirror-bin PRIVATE ftxui::component)

target_link_libraries(ai-mirror-bin PRIVATE pthread)

find_package(OpenSSL REQUIRED)
target_link_libraries(ai-mirror-bin PRIVATE OpenSSL::Crypto)

if(EXISTS "${CMAKE_SOURCE_DIR}/tests")
    enable_testing()
    set(COMMON_SOURCES ${AI_MIRROR_SOURCES})
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

    # Unit tests for watch_stats (standalone binary, no main.cpp)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/unit/test_watch_stats.cpp")
        add_executable(test_watch_stats tests/unit/test_watch_stats.cpp
            src/daemon/watch_stats.cpp
            src/utils/shell.cpp
            src/utils/logger.cpp
            src/security/path_validator.cpp
        )
        target_include_directories(test_watch_stats PRIVATE ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(test_watch_stats PRIVATE
            CLI11::CLI11
            toml11::toml11
            spdlog::spdlog
            nlohmann_json::nlohmann_json
            pthread
        )
        target_compile_options(test_watch_stats PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
        )
        target_link_options(test_watch_stats PRIVATE
            -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
        )
        add_test(NAME test_watch_stats COMMAND test_watch_stats)
    endif()

    # Additional tests can be added here following the same pattern

    # Unit tests for parse_known_hosts_hosts (standalone binary)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/unit/test_known_hosts.cpp")
        add_executable(test_known_hosts tests/unit/test_known_hosts.cpp)
        target_include_directories(test_known_hosts PRIVATE ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(test_known_hosts PRIVATE
            CLI11::CLI11
            toml11::toml11
            spdlog::spdlog
            nlohmann_json::nlohmann_json
            pthread
        )
        target_compile_options(test_known_hosts PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
        )
        target_link_options(test_known_hosts PRIVATE
            -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
        )
        add_test(NAME test_known_hosts COMMAND test_known_hosts)
    endif()
endif()

cmake_minimum_required(VERSION 3.28)
project(ai-mirror VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(PkgConfig)

add_executable(ai-mirror
    src/main.cpp
    src/cli/parser.cpp
    src/cli/commands.cpp
    src/core/user_manager.cpp
    src/core/graft.cpp
    src/core/ssh_manager.cpp
    src/core/path_resolver.cpp
    src/core/config.cpp
    src/daemon/health_check.cpp
    src/daemon/auth_monitor.cpp
    src/daemon/mount_cleaner.cpp
    src/security/path_validator.cpp
    src/security/audit.cpp
    src/utils/shell.cpp
    src/utils/logger.cpp
)

target_include_directories(ai-mirror PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_options(ai-mirror PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -Wformat -Wformat-security
    -fvisibility=hidden
)

target_link_options(ai-mirror PRIVATE
    -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
)

# CLI11 - header-only CLI library
include(FetchContent)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(cli11)
target_link_libraries(ai-mirror PRIVATE CLI11::CLI11)

# toml11 - header-only TOML parser
FetchContent_Declare(
    toml11
    GIT_REPOSITORY https://github.com/ToruNiina/toml11.git
    GIT_TAG v4.2.0
)
FetchContent_MakeAvailable(toml11)
target_link_libraries(ai-mirror PRIVATE toml11::toml11)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
)
FetchContent_MakeAvailable(spdlog)
target_link_libraries(ai-mirror PRIVATE spdlog::spdlog)

# nlohmann_json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)
target_link_libraries(ai-mirror PRIVATE nlohmann_json::nlohmann_json)

target_link_libraries(ai-mirror PRIVATE
    pthread
)

# Tests
enable_testing()

set(TEST_LIBS
    CLI11::CLI11
    toml11::toml11
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    pthread
)

set(TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

set(COMMON_TEST_SOURCES
    src/utils/shell.cpp
    src/utils/logger.cpp
)

macro(add_ai_mirror_test name)
    add_executable(${name} ${ARGN} ${COMMON_TEST_SOURCES})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${name} PRIVATE ${TEST_LIBS})
    target_compile_options(${name} PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
        -Wformat -Wformat-security
    )
    target_link_options(${name} PRIVATE
        -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
    )
    add_test(NAME ${name} COMMAND ${name})
endmacro()

add_ai_mirror_test(test-path-validator
    tests/test_path_validator.cpp
    src/security/path_validator.cpp
)

add_ai_mirror_test(test-user-manager
    tests/test_user_manager.cpp
    src/core/user_manager.cpp
    src/security/path_validator.cpp
)

add_ai_mirror_test(test-graft
    tests/test_graft.cpp
    src/core/graft.cpp
    src/security/path_validator.cpp
)

add_ai_mirror_test(test-config
    tests/test_config.cpp
    src/core/config.cpp
)

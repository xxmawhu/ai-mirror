cmake_minimum_required(VERSION 3.28)
project(ai-mirror VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
set(CMAKE_LINK_SEARCH_START_STATIC ON)
set(CMAKE_LINK_SEARCH_END_STATIC ON)

find_package(PkgConfig)

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

add_executable(ai-mirror ${AI_MIRROR_SOURCES})

target_include_directories(ai-mirror PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(ai-mirror PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -Wformat -Wformat-security
    -fvisibility=hidden
    -ffunction-sections -fdata-sections
)

target_link_options(ai-mirror PRIVATE
    -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
    -static-libstdc++ -static-libgcc
    -Wl,--gc-sections -Wl,--strip-all
)

include(FetchContent)

FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(cli11)
target_link_libraries(ai-mirror PRIVATE CLI11::CLI11)

FetchContent_Declare(toml11
    URL "https://github.com/ToruNiina/toml11/archive/refs/tags/v4.2.0.tar.gz"
)
FetchContent_MakeAvailable(toml11)
target_link_libraries(ai-mirror PRIVATE toml11::toml11)

FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
)
FetchContent_MakeAvailable(spdlog)
target_link_libraries(ai-mirror PRIVATE spdlog::spdlog)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)
target_link_libraries(ai-mirror PRIVATE nlohmann_json::nlohmann_json)

FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG v6.1.9
)
FetchContent_MakeAvailable(ftxui)
target_link_libraries(ai-mirror PRIVATE ftxui::component)

target_link_libraries(ai-mirror PRIVATE pthread)

find_package(OpenSSL REQUIRED)
target_link_libraries(ai-mirror PRIVATE OpenSSL::Crypto)

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

cmake_minimum_required(VERSION 3.28)
project(ai-mirror VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(PkgConfig)

file(GLOB_RECURSE AI_MIRROR_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(ai-mirror ${AI_MIRROR_SOURCES})

target_include_directories(ai-mirror PRIVATE
    ${CMAKE_SOURCE_DIR}/include
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

include(FetchContent)

FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(cli11)
target_link_libraries(ai-mirror PRIVATE CLI11::CLI11)

FetchContent_Declare(toml11
    URL "/tmp/toml11-4.2.0.tar.gz"
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

target_link_libraries(ai-mirror PRIVATE pthread)

if(EXISTS "${CMAKE_SOURCE_DIR}/tests")
    enable_testing()
    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
    set(COMMON_SOURCES ${AI_MIRROR_SOURCES})
    list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

    foreach(test_file ${TEST_SOURCES})
        get_filename_component(test_name ${test_file} NAME_WE)
        add_executable(${test_name} ${test_file} ${COMMON_SOURCES})
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(${test_name} PRIVATE CLI11::CLI11 toml11::toml11 spdlog::spdlog nlohmann_json::nlohmann_json pthread)
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic -Werror -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2)
        target_link_options(${test_name} PRIVATE -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

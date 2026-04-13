#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "ai_mirror/security/path_validator.hpp"

namespace fs = std::filesystem;

static void test_is_subpath() {
    assert(ai_mirror::security::is_subpath("/home", "/home/maxx"));
    assert(ai_mirror::security::is_subpath("/home", "/home"));
    assert(!ai_mirror::security::is_subpath("/home/maxx", "/home"));
    assert(!ai_mirror::security::is_subpath("/home", "/home_other"));
    std::cout << "  [PASS] is_subpath" << std::endl;
}

static void test_validate_mount_paths_empty() {
    auto result = ai_mirror::security::validate_mount_paths("", "/tmp/target");
    assert(!result.safe);
    assert(result.reason.find("empty") != std::string::npos);
    std::cout << "  [PASS] validate_mount_paths empty" << std::endl;
}

static void test_validate_mount_paths_same() {
    auto result = ai_mirror::security::validate_mount_paths("/home/maxx", "/home/maxx");
    assert(!result.safe);
    assert(result.reason.find("same") != std::string::npos);
    std::cout << "  [PASS] validate_mount_paths same path" << std::endl;
}

static void test_validate_mount_paths_source_not_exists() {
    auto result = ai_mirror::security::validate_mount_paths("/nonexistent/path/xyz", "/tmp/target");
    assert(!result.safe);
    assert(result.reason.find("does not exist") != std::string::npos);
    std::cout << "  [PASS] validate_mount_paths source not exists" << std::endl;
}

static void test_validate_mount_paths_valid() {
    auto result = ai_mirror::security::validate_mount_paths("/etc/hostname", "/tmp/ai-mirror-test-target");
    assert(result.safe);
    std::cout << "  [PASS] validate_mount_paths valid" << std::endl;
}

int main() {
    std::cout << "=== Path Validator Tests ===" << std::endl;
    test_is_subpath();
    test_validate_mount_paths_empty();
    test_validate_mount_paths_same();
    test_validate_mount_paths_source_not_exists();
    test_validate_mount_paths_valid();
    std::cout << "All path_validator tests passed!" << std::endl;
    return 0;
}

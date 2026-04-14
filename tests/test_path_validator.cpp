#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "ai_mirror/security/path_validator.hpp"
#include "ai_mirror/utils/shell.hpp"

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

static void test_safe_canonical_empty_on_failure() {
    auto result = ai_mirror::security::safe_canonical("/nonexistent/path/that/does/not/exist");
    assert(result.empty());
    std::cout << "  [PASS] safe_canonical returns empty on double failure" << std::endl;
}

static void test_safe_canonical_rejects_dotdot() {
    auto result = ai_mirror::security::safe_canonical("/home/../etc");
    assert(result.empty());
    std::cout << "  [PASS] safe_canonical rejects '..' components" << std::endl;
}

static void test_validate_path_exists_real() {
    assert(ai_mirror::security::validate_path_exists("/tmp"));
    assert(ai_mirror::security::validate_path_exists("/etc/hostname"));
    std::cout << "  [PASS] validate_path_exists real paths" << std::endl;
}

static void test_validate_path_exists_empty() {
    assert(!ai_mirror::security::validate_path_exists(""));
    assert(!ai_mirror::security::validate_path_exists("/nonexistent/xyz123"));
    std::cout << "  [PASS] validate_path_exists empty/nonexistent" << std::endl;
}

static void test_metachar_rejection() {
    using ai_mirror::utils::validate_path_no_shell_metachars;
    assert(!validate_path_no_shell_metachars("/home/user;rm -rf /"));
    assert(!validate_path_no_shell_metachars("/home/user$(evil)"));
    assert(!validate_path_no_shell_metachars("/home/user`cmd`"));
    assert(!validate_path_no_shell_metachars("/home/user|pipe"));
    assert(!validate_path_no_shell_metachars("/home/user&bg"));
    assert(validate_path_no_shell_metachars("/home/user/project"));
    assert(validate_path_no_shell_metachars("/home/user/my-project.v2"));
    std::cout << "  [PASS] validate_path_no_shell_metachars" << std::endl;
}

static void test_is_path_allowed_canonical_failure() {
    assert(!ai_mirror::utils::is_path_allowed("/nonexistent/path/xyz", "root"));
    std::cout << "  [PASS] is_path_allowed rejects nonexistent (no fallback)" << std::endl;
}

static void test_is_path_allowed_dotdot() {
    assert(!ai_mirror::utils::is_path_allowed(fs::path("/home/user/../etc"), "user"));
    std::cout << "  [PASS] is_path_allowed rejects '..' components" << std::endl;
}

int main() {
    std::cout << "=== Path Validator Tests ===" << std::endl;
    test_is_subpath();
    test_validate_mount_paths_empty();
    test_validate_mount_paths_same();
    test_validate_mount_paths_source_not_exists();
    test_validate_mount_paths_valid();
    test_safe_canonical_empty_on_failure();
    test_safe_canonical_rejects_dotdot();
    test_validate_path_exists_real();
    test_validate_path_exists_empty();
    test_metachar_rejection();
    test_is_path_allowed_canonical_failure();
    test_is_path_allowed_dotdot();
    std::cout << "All path_validator tests passed!" << std::endl;
    return 0;
}

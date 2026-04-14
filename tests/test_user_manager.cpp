#include <cassert>
#include <iostream>
#include <string>

#include "ai_mirror/core/user_manager.hpp"

static void test_derive_username() {
    ai_mirror::core::UserManager mgr("i");
    std::string username = mgr.derive_username("/home/maxx/projects/alpha");
    assert(username.find("imaxx") != std::string::npos);
    assert(username.find("alpha") != std::string::npos);
    assert(username.length() <= 32);
    std::cout << "  [PASS] derive_username: " << username << std::endl;
}

static void test_derive_username_special_chars() {
    ai_mirror::core::UserManager mgr("i");
    std::string username = mgr.derive_username("/home/maxx/projects/my-project.v2");
    assert(username.find('-') == std::string::npos);
    assert(username.find('.') == std::string::npos);
    std::cout << "  [PASS] derive_username special chars: " << username << std::endl;
}

static void test_get_prefix() {
    ai_mirror::core::UserManager mgr("i");
    assert(mgr.get_prefix() == "i");
    std::cout << "  [PASS] get_prefix" << std::endl;
}

static void test_derive_username_length() {
    ai_mirror::core::UserManager mgr("i");
    std::string long_path = "/home/maxx/projects/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string username = mgr.derive_username(long_path);
    assert(username.length() <= 32);
    assert(username.find("imaxx") != std::string::npos);
    std::cout << "  [PASS] derive_username truncated to 32: " << username << " (len=" << username.length() << ")" << std::endl;
}

static void test_derive_username_collision_different() {
    ai_mirror::core::UserManager mgr("i");
    std::string path_a = "/home/maxx/projects/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_first";
    std::string path_b = "/home/maxx/projects/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_second";
    std::string ua = mgr.derive_username(path_a);
    std::string ub = mgr.derive_username(path_b);
    assert(ua != ub);
    assert(ua.length() <= 32);
    assert(ub.length() <= 32);
    std::cout << "  [PASS] derive_username different for similar long names: " << ua << " vs " << ub << std::endl;
}

int main() {
    std::cout << "=== User Manager Tests ===" << std::endl;
    test_derive_username();
    test_derive_username_special_chars();
    test_get_prefix();
    test_derive_username_length();
    test_derive_username_collision_different();
    std::cout << "All user_manager tests passed!" << std::endl;
    return 0;
}

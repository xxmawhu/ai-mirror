#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <cstdlib>

#include "ai_mirror/core/config.hpp"

namespace fs = std::filesystem;

static std::string write_test_config(const std::string& content) {
    std::string tmpdir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    std::string path = tmpdir + "/ai-mirror-test-config-" + std::to_string(::getpid()) + ".toml";
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
    return path;
}

static void test_load_default() {
    auto config = ai_mirror::core::ConfigParser::load_default();
    assert(!config.ssh.key_type.empty());
    std::cout << "  [PASS] load_default (key_type=" << config.ssh.key_type << ")" << std::endl;
}

static void test_load_from_file() {
    std::string content = R"(
[ssh]
key_type = "rsa"
ai_default_key = "/home/test/.ssh/id_ed25519.pub"
)";
    std::string path = write_test_config(content);
    auto config = ai_mirror::core::ConfigParser::load(path);
    assert(config.ssh.key_type == "rsa");
    assert(config.ssh.ai_default_key == fs::path("/home/test/.ssh/id_ed25519.pub"));
    std::filesystem::remove(path);
    std::cout << "  [PASS] load_from_file" << std::endl;
}

static void test_save_and_load() {
    std::string tmpdir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    std::string path = tmpdir + "/ai-mirror-test-save-" + std::to_string(::getpid()) + ".toml";

    auto config = ai_mirror::core::ConfigParser::create_default_config(path);
    config.ssh.key_type = "ed25519";
    assert(ai_mirror::core::ConfigParser::save(config, path));

    auto loaded = ai_mirror::core::ConfigParser::load(path);
    assert(loaded.ssh.key_type == "ed25519");
    std::filesystem::remove(path);
    std::cout << "  [PASS] save_and_load" << std::endl;
}

int main() {
    std::cout << "=== Config Tests ===" << std::endl;
    test_load_default();
    test_load_from_file();
    test_save_and_load();
    std::cout << "All config tests passed!" << std::endl;
    return 0;
}

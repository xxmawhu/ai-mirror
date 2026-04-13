#include <cassert>
#include <iostream>

#include "ai_mirror/core/graft.hpp"

static void test_graft_default_construction() {
    ai_mirror::core::Graft graft;
    std::cout << "  [PASS] Graft default construction" << std::endl;
}

static void test_graft_mount_entry_fields() {
    ai_mirror::core::MountEntry entry;
    entry.source = "/home/maxx/.bashrc";
    entry.target = "/home/imaxx_alpha/.bashrc";
    entry.read_only = true;
    entry.active = true;
    assert(entry.read_only);
    assert(entry.active);
    std::cout << "  [PASS] MountEntry fields" << std::endl;
}

int main() {
    std::cout << "=== Graft Tests ===" << std::endl;
    test_graft_default_construction();
    test_graft_mount_entry_fields();
    std::cout << "All graft tests passed!" << std::endl;
    return 0;
}

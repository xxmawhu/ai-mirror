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

static void test_revoke_rejects_invalid_username() {
    ai_mirror::core::Graft graft;
    assert(!graft.revoke_write_access("/tmp", "bad;user"));
    assert(!graft.revoke_write_access("/tmp", ""));
    std::cout << "  [PASS] revoke_write_access rejects invalid username" << std::endl;
}

static void test_grant_rejects_invalid_username() {
    ai_mirror::core::Graft graft;
    assert(!graft.grant_write_access("/tmp", "bad;user"));
    assert(!graft.grant_write_access("/tmp", ""));
    std::cout << "  [PASS] grant_write_access rejects invalid username" << std::endl;
}

int main() {
    std::cout << "=== Graft Tests ===" << std::endl;
    test_graft_default_construction();
    test_graft_mount_entry_fields();
    test_revoke_rejects_invalid_username();
    test_grant_rejects_invalid_username();
    std::cout << "All graft tests passed!" << std::endl;
    return 0;
}

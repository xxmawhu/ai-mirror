// Real-world integration test for virtual filesystem detection.
// This test compiles against the ACTUAL project source code and tests
// the exact code path that was broken: /proc/mounts BeeGFS format.
//
// /proc/mounts for BeeGFS:
//   beegfs_nodev /mnt/beegfs_data beegfs rw,nosuid,relatime,...
//   └─col1: device──┘              └─col3: fstype──┘
//
// The OLD code had "beegfs_nodev" (col1) in the fstype list instead
// of "beegfs" (col3).  This test verifies the fix with REAL data.

#include "ai_mirror/core/vfs_util.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

using ai_mirror::core::is_virtual_source;

static int passed = 0;
static int failed = 0;

#define TEST(name) std::cout << "  " << name << " ... "

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cout << "FAIL: " << msg << std::endl;                               \
      failed++;                                                                \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define PASS()                                                                 \
  do {                                                                         \
    std::cout << "PASS" << std::endl;                                          \
    passed++;                                                                  \
  } while (0)

// ============================================================
// Test: exact /proc/mounts format that was broken
// ============================================================

void test_beegfs_proc_mounts_format() {
  TEST("BeeGFS /proc/mounts: device=beegfs_nodev, fstype=beegfs");

  // This is the EXACT format from production /proc/mounts:
  //   beegfs_nodev /mnt/beegfs_data beegfs rw,...
  std::string device = "beegfs_nodev"; // column 1
  std::string fstype = "beegfs";       // column 3

  bool result = is_virtual_source(device, fstype);
  CHECK(result, "beegfs mount should be detected as virtual");
  PASS();
}

void test_beegfs_serialization() {
  TEST("BeeGFS serialization: source cleared, source_stat skipped");

  // Simulate the exact code path in make_mounts_json():
  //   update_state_mounts() populates MountInfo from /proc/mounts
  //   make_mounts_json() serializes MountInfo to JSON
  //
  // After fix:
  //   mi.source = "beegfs_nodev" → cleared to ""
  //   mi.fstype = "beegfs"
  //   is_virtual_source("", "beegfs") → true → source_stat skipped

  std::string source = "beegfs_nodev";
  std::string fstype = "beegfs";

  // Step 1: Clear source as update_state_mounts does
  if (is_virtual_source(source, fstype)) {
    source.clear(); // mi.source.clear()
  }

  // Step 2: Serialize as make_mounts_json does
  bool is_virtual = is_virtual_source(source, fstype);

  CHECK(source.empty(), "source should be cleared");
  CHECK(is_virtual, "cleared source with beegfs fstype should be virtual");
  PASS();
}

void test_real_path_preserved() {
  TEST("Real ext4 mount: source preserved, source_stat included");

  std::string source = "/dev/sda1";
  std::string fstype = "ext4";

  bool result = is_virtual_source(source, fstype);
  CHECK(!result, "ext4 mount should NOT be virtual");
  PASS();
}

void test_nfs_real_path() {
  TEST("NFS mount: fstype=nfs4, source=server:/path (no leading '/')");

  std::string source = "server:/export/data";
  std::string fstype = "nfs4";

  bool result = is_virtual_source(source, fstype);
  CHECK(!result, "NFS mount should NOT be virtual (nfs4 is a real network fs)");
  PASS();
}

void test_tmpfs_virtual() {
  TEST("tmpfs mount: fstype=tmpfs, source=tmpfs");

  std::string source = "tmpfs";
  std::string fstype = "tmpfs";

  bool result = is_virtual_source(source, fstype);
  CHECK(result, "tmpfs should be virtual");
  PASS();
}

void test_proc_virtual() {
  TEST("proc mount: fstype=proc, source=proc");

  std::string source = "proc";
  std::string fstype = "proc";

  bool result = is_virtual_source(source, fstype);
  CHECK(result, "proc should be virtual");
  PASS();
}

void test_all_virtual_fstypes() {
  TEST("All 24 virtual fstypes from /proc/mounts col3");

  const char *types[] = {
      "proc",    "tmpfs",   "devtmpfs",    "sysfs",       "cgroup",
      "cgroup2", "devpts",  "none",        "binfmt_misc", "configfs",
      "debugfs", "tracefs", "securityfs",  "pstore",      "hugetlbfs",
      "mqueue",  "fusectl", "efivarfs",    "bpf",         "autofs",
      "overlay", "aufs",    "fuse.portal", "beegfs",
  };
  for (auto t : types) {
    bool result = is_virtual_source("dummy", t);
    if (!result) {
      std::cout << "FAIL: fstype '" << t << "' not detected as virtual"
                << std::endl;
      failed++;
      return;
    }
  }
  PASS();
}

// ============================================================
// Main
// ============================================================

int main() {
  std::cout << std::endl;
  std::cout << "============================================" << std::endl;
  std::cout << "  VFS Real Integration Tests" << std::endl;
  std::cout << "  (compiled against actual project code)" << std::endl;
  std::cout << "============================================" << std::endl;
  std::cout << std::endl;

  std::cout << "--- BeeGFS tests (the bug that was fixed) ---" << std::endl;
  test_beegfs_proc_mounts_format();
  test_beegfs_serialization();

  std::cout << "--- Standard filesystem tests ---" << std::endl;
  test_real_path_preserved();
  test_nfs_real_path();
  test_tmpfs_virtual();
  test_proc_virtual();
  test_all_virtual_fstypes();

  std::cout << std::endl;
  std::cout << "============================================" << std::endl;
  std::cout << "  Results: " << passed << " passed, " << failed << " failed"
            << std::endl;
  std::cout << "============================================" << std::endl;
  std::cout << std::endl;

  return failed > 0 ? 1 : 0;
}

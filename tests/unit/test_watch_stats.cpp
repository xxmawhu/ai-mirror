// Unit tests for watch_stats: /proc parsing, stats gathering
// Compile as standalone binary (no test framework, assert-based)
#include "ai_mirror/daemon/watch_stats.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static int passed = 0;
static int failed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    std::cout << "  TEST: " << name << " ... ";                                \
  } while (0)

#define PASS()                                                                 \
  do {                                                                         \
    std::cout << "PASS" << std::endl;                                          \
    passed++;                                                                  \
  } while (0)

#define FAIL(msg)                                                              \
  do {                                                                         \
    std::cout << "FAIL: " << msg << std::endl;                                 \
    failed++;                                                                  \
  } while (0)

#define ASSERT_TRUE(expr, msg)                                                 \
  do {                                                                         \
    if (!(expr)) {                                                             \
      FAIL(msg);                                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::ostringstream _ss;                                                  \
      _ss << msg << " (got " << (a) << ", expected " << (b) << ")";            \
      FAIL(_ss.str());                                                         \
      return;                                                                  \
    }                                                                          \
  } while (0)

// ========================================================================
// Test: read_proc_status with valid file
// ========================================================================
void test_read_proc_status_valid() {
  TEST("read_proc_status: valid file");

  // Create temp directory with fake proc status
  fs::path tmp_dir = fs::temp_directory_path() / "ai_mirror_test_proc";
  fs::remove_all(tmp_dir);
  fs::create_directories(tmp_dir);

  uid_t test_uid = getuid();
  unsigned long test_rss = 12345;

  // Create fake status file directly
  fs::path status_file = tmp_dir / "status";
  std::ofstream f(status_file);
  f << "Name:   test_proc\n";
  f << "Uid:    " << test_uid << "\n";
  f << "VmRSS:  " << test_rss << " kB\n";
  f.close();

  auto result = ai_mirror::daemon::read_proc_status(status_file);
  ASSERT_TRUE(result.has_value(), "should return a value");
  ASSERT_EQ(result->uid, test_uid, "UID mismatch");
  ASSERT_EQ(result->vm_rss_kb, test_rss, "VmRSS mismatch");

  fs::remove_all(tmp_dir);
  PASS();
}

// ========================================================================
// Test: read_proc_status with nonexistent file
// ========================================================================
void test_read_proc_status_nonexistent() {
  TEST("read_proc_status: nonexistent file");

  auto result = ai_mirror::daemon::read_proc_status("/nonexistent/path/status");
  ASSERT_TRUE(!result.has_value(),
              "should return nullopt for nonexistent file");
  PASS();
}

// ========================================================================
// Test: read_proc_status with file missing Uid line
// ========================================================================
void test_read_proc_status_no_uid() {
  TEST("read_proc_status: missing Uid line");

  fs::path tmp_dir = fs::temp_directory_path() / "ai_mirror_test_no_uid";
  fs::remove_all(tmp_dir);
  fs::create_directories(tmp_dir);

  fs::path status_file = tmp_dir / "status";
  std::ofstream f(status_file);
  f << "Name:   test_proc\n";
  f << "VmRSS:  1000 kB\n";
  f.close();

  auto result = ai_mirror::daemon::read_proc_status(status_file);
  ASSERT_TRUE(!result.has_value(), "should return nullopt without Uid");

  fs::remove_all(tmp_dir);
  PASS();
}

// ========================================================================
// Test: read_proc_status with file having only Uid
// ========================================================================
void test_read_proc_status_only_uid() {
  TEST("read_proc_status: only Uid, no VmRSS");

  fs::path tmp_dir = fs::temp_directory_path() / "ai_mirror_test_only_uid";
  fs::remove_all(tmp_dir);
  fs::create_directories(tmp_dir);

  fs::path status_file = tmp_dir / "status";
  std::ofstream f(status_file);
  f << "Name:   test_proc\n";
  f << "Uid:    1001\n";
  f.close();

  auto result = ai_mirror::daemon::read_proc_status(status_file);
  ASSERT_TRUE(result.has_value(), "should return a value");
  ASSERT_EQ(result->uid, static_cast<uid_t>(1001), "UID should be 1001");
  ASSERT_EQ(result->vm_rss_kb, static_cast<unsigned long>(0),
            "VmRSS should default to 0");

  fs::remove_all(tmp_dir);
  PASS();
}

// ========================================================================
// Test: gather_user_stats with empty inputs
// ========================================================================
void test_gather_user_stats_empty() {
  TEST("gather_user_stats: empty input");

  auto stats = ai_mirror::daemon::gather_user_stats({}, {});
  ASSERT_EQ(stats.size(), static_cast<size_t>(0), "should return empty vector");
  PASS();
}

// ========================================================================
// Test: gather_user_stats with mismatched sizes
// ========================================================================
void test_gather_user_stats_mismatch() {
  TEST("gather_user_stats: mismatched sizes");

  auto stats = ai_mirror::daemon::gather_user_stats({"user1"}, {});
  ASSERT_EQ(stats.size(), static_cast<size_t>(0),
            "should return empty for mismatched sizes");
  PASS();
}

// ========================================================================
// Test: gather_all_uid_stats returns valid map
// ========================================================================
void test_gather_all_uid_stats() {
  TEST("gather_all_uid_stats: returns valid map");

  auto stats = ai_mirror::daemon::gather_all_uid_stats();

  // Should return a map (may be empty if no processes match, but not crash)
  // At minimum, current user's UID should appear
  uid_t my_uid = getuid();
  bool found_self = (stats.find(my_uid) != stats.end());

  if (found_self) {
    auto &my_stats = stats.at(my_uid);
    ASSERT_TRUE(my_stats.process_count >= 0, "process_count should be >= 0");
    // We're running, so at least 1 process
    ASSERT_TRUE(my_stats.process_count >= 1, "should have at least 1 process");
  }
  // If not found, that's also OK (some systems may not report our UID)

  PASS();
}

// ========================================================================
// Test: build_user_stats with valid uid_stats
// ========================================================================
void test_build_user_stats_from_map() {
  TEST("build_user_stats: from uid_stats map");

  // Create a fake uid_stats map
  std::unordered_map<uid_t, ai_mirror::daemon::UidStats> uid_stats;
  ai_mirror::daemon::UidStats fake_stats;
  fake_stats.process_count = 3;
  fake_stats.memory_mb = 128;
  fake_stats.cpu_percent = 5.5;
  fake_stats.has_sshd = true;
  uid_stats[88888] = fake_stats;

  auto result =
      ai_mirror::daemon::build_user_stats({"test_user"}, {88888}, uid_stats);

  ASSERT_EQ(result.size(), static_cast<size_t>(1), "should return 1 entry");
  ASSERT_EQ(result[0].username, "test_user", "username mismatch");
  ASSERT_EQ(result[0].process_count, 3, "process_count mismatch");
  ASSERT_EQ(result[0].memory_mb, static_cast<unsigned long>(128),
            "memory mismatch");
  ASSERT_TRUE(result[0].logged_in, "should be logged in");
  PASS();
}

// ========================================================================
// Test: build_user_stats with missing uid in map
// ========================================================================
void test_build_user_stats_missing_uid() {
  TEST("build_user_stats: missing uid defaults");

  std::unordered_map<uid_t, ai_mirror::daemon::UidStats> uid_stats;
  // Empty map - user 88888 not in it

  auto result =
      ai_mirror::daemon::build_user_stats({"missing_user"}, {88888}, uid_stats);

  ASSERT_EQ(result.size(), static_cast<size_t>(1), "should return 1 entry");
  ASSERT_EQ(result[0].process_count, 0, "process_count should default to 0");
  ASSERT_EQ(result[0].memory_mb, static_cast<unsigned long>(0),
            "memory should default to 0");
  ASSERT_EQ(result[0].cpu_percent, 0.0, "cpu should default to 0");
  ASSERT_TRUE(!result[0].logged_in, "logged_in should default to false");
  PASS();
}

// ========================================================================
// Main
// ========================================================================
int main() {
  std::cout << "=== watch_stats unit tests ===" << std::endl;

  test_read_proc_status_valid();
  test_read_proc_status_nonexistent();
  test_read_proc_status_no_uid();
  test_read_proc_status_only_uid();
  test_gather_user_stats_empty();
  test_gather_user_stats_mismatch();
  test_gather_all_uid_stats();
  test_build_user_stats_from_map();
  test_build_user_stats_missing_uid();

  std::cout << std::endl;
  std::cout << "Results: " << passed << " passed, " << failed << " failed"
            << std::endl;

  return failed > 0 ? 1 : 0;
}

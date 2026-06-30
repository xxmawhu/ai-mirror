// Unit tests for mount stat logic: virtual device detection, source_stat
// serialization, and health check skipping.
//
// Tests the core algorithm used in mount_watch.cpp and user_manager.cpp:
//   1. is_virtual_device detection (source[0] != '/')
//   2. source_stat serialization skipping for virtual devices
//   3. MountInfo JSON output format
//
// Compile as standalone binary (no test framework, assert-based).
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

#define ASSERT_FALSE(expr, msg)                                                \
  do {                                                                         \
    if ((expr)) {                                                              \
      FAIL(msg);                                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::ostringstream _ss;                                                  \
      _ss << msg << " (got '" << (a) << "', expected '" << (b) << "')";        \
      FAIL(_ss.str());                                                         \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NE(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) == (b)) {                                                          \
      std::ostringstream _ss;                                                  \
      _ss << msg << " (both are " << (a) << ")";                               \
      FAIL(_ss.str());                                                         \
      return;                                                                  \
    }                                                                          \
  } while (0)

// ---- Inline version of the virtual device detection logic ----

/// Returns true if source is a virtual device name (beegfs_nodev, tmpfs, etc.)
static bool is_virtual_source(const std::string &source) {
  return source.empty() || source[0] != '/';
}

/// Minimal MountStatInfo matching the real struct
struct MountStatInfo {
  ino_t ino = 0;
  dev_t dev = 0;
  mode_t mode = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  off_t size = 0;
  time_t mtime = 0;
};

/// Minimal MountInfo matching the real struct
struct MountInfo {
  std::string source;
  std::string target;
  bool read_only = true;
  MountStatInfo source_stat;
};

/// Serialize a single MountInfo to JSON string (inline version of
/// make_mounts_json logic)
static std::string mount_to_json(const MountInfo &m) {
  std::string s = "    {\n";
  s += "      \"source\": \"" + m.source + "\",\n";
  s += "      \"target\": \"" + m.target + "\",\n";
  s += "      \"read_only\": " + std::string(m.read_only ? "true" : "false");
  bool is_virtual = is_virtual_source(m.source);
  if (!is_virtual) {
    s += ",\n";
    s += "      \"source_stat\": {\n";
    s += "        \"ino\": " + std::to_string(m.source_stat.ino) + ",\n";
    s += "        \"dev\": " + std::to_string(m.source_stat.dev) + ",\n";
    s += "        \"mode\": " + std::to_string(m.source_stat.mode) + ",\n";
    s += "        \"uid\": " + std::to_string(m.source_stat.uid) + ",\n";
    s += "        \"gid\": " + std::to_string(m.source_stat.gid) + ",\n";
    s += "        \"size\": " + std::to_string(m.source_stat.size) + ",\n";
    s += "        \"mtime\": " + std::to_string(m.source_stat.mtime) + "\n";
    s += "      }\n";
  } else {
    s += "\n";
  }
  s += "    }";
  return s;
}

// ================================================================
// Test: is_virtual_source detects common virtual device names
// ================================================================

void test_virtual_beegfs() {
  TEST("is_virtual_source: beegfs_nodev");
  ASSERT_TRUE(is_virtual_source("beegfs_nodev"),
              "beegfs_nodev should be virtual");
  PASS();
}

void test_virtual_tmpfs() {
  TEST("is_virtual_source: tmpfs");
  ASSERT_TRUE(is_virtual_source("tmpfs"), "tmpfs should be virtual");
  PASS();
}

void test_virtual_proc() {
  TEST("is_virtual_source: proc");
  ASSERT_TRUE(is_virtual_source("proc"), "proc should be virtual");
  PASS();
}

void test_virtual_empty() {
  TEST("is_virtual_source: empty string");
  ASSERT_TRUE(is_virtual_source(""), "empty should be virtual");
  PASS();
}

void test_real_path() {
  TEST("is_virtual_source: /real/path");
  ASSERT_FALSE(is_virtual_source("/real/path"),
               "/real/path should NOT be virtual");
  PASS();
}

void test_real_root() {
  TEST("is_virtual_source: root path /");
  ASSERT_FALSE(is_virtual_source("/"), "root / should NOT be virtual");
  PASS();
}

// ================================================================
// Test: serialization skips source_stat for virtual devices
// ================================================================

void test_serialize_virtual_no_source_stat() {
  TEST("serialize: virtual mount has NO source_stat");

  MountInfo m;
  m.source = "beegfs_nodev";
  m.target = "/tmp/test/.bashrc";
  m.read_only = true;
  // source_stat intentionally left as all zeros

  std::string json = mount_to_json(m);

  // Must NOT contain "source_stat"
  ASSERT_TRUE(json.find("source_stat") == std::string::npos,
              "virtual mount should not have source_stat in JSON");

  // Must contain source, target, read_only
  ASSERT_NE(json.find("\"source\": \"beegfs_nodev\""), std::string::npos,
            "JSON should contain source field");
  ASSERT_NE(json.find("\"target\": \"/tmp/test/.bashrc\""), std::string::npos,
            "JSON should contain target field");
  ASSERT_NE(json.find("\"read_only\": true"), std::string::npos,
            "JSON should contain read_only field");

  PASS();
}

void test_serialize_real_has_source_stat() {
  TEST("serialize: real path mount HAS source_stat");

  MountInfo m;
  m.source = "/real/path/file";
  m.target = "/tmp/test/.bashrc";
  m.read_only = false;
  m.source_stat.ino = 12345;
  m.source_stat.dev = 42;
  m.source_stat.mode = 0644;
  m.source_stat.uid = 1000;
  m.source_stat.gid = 1000;
  m.source_stat.size = 4096;
  m.source_stat.mtime = 1234567890;

  std::string json = mount_to_json(m);

  // Must contain source_stat
  ASSERT_NE(json.find("\"source_stat\""), std::string::npos,
            "real path mount should have source_stat in JSON");

  // Must contain actual stat values
  ASSERT_NE(json.find("\"ino\": 12345"), std::string::npos,
            "JSON should contain ino");
  ASSERT_NE(json.find("\"dev\": 42"), std::string::npos,
            "JSON should contain dev");
  ASSERT_NE(json.find("\"mode\": 420"), std::string::npos,
            "JSON should contain mode (0644 = 420 decimal)");

  PASS();
}

void test_serialize_empty_source_virtual() {
  TEST("serialize: empty source is treated as virtual");

  MountInfo m;
  m.source = ""; // cleared by update_state_mounts for virtual devices

  std::string json = mount_to_json(m);

  // Empty source should also skip source_stat
  ASSERT_TRUE(json.find("source_stat") == std::string::npos,
              "empty source mount should not have source_stat");

  PASS();
}

void test_serialize_real_zero_stat_included() {
  TEST("serialize: real path with zero stat still includes source_stat");

  MountInfo m;
  m.source = "/real/but/zero/stat";
  // source_stat intentionally all zeros (real path where stat failed)

  std::string json = mount_to_json(m);

  // Real path mount must ALWAYS include source_stat, even if zero
  // (zero stat is userful debugging info for a failed stat)
  ASSERT_NE(json.find("\"source_stat\""), std::string::npos,
            "real path mount must include source_stat even if zero");
  ASSERT_NE(json.find("\"ino\": 0"), std::string::npos,
            "JSON should show ino=0 for failed stat");

  PASS();
}

// ================================================================
// Test: stat(2) on real vs virtual paths
// ================================================================

void test_stat_real_path() {
  TEST("stat: real temp file succeeds");

  fs::path tmp_dir = fs::temp_directory_path() / "ai_mirror_test_mount_stat";
  fs::remove_all(tmp_dir);
  fs::create_directories(tmp_dir);
  fs::path tmp_file = tmp_dir / "test_file";
  {
    std::ofstream f(tmp_file);
    f << "test content" << std::endl;
  }

  struct stat st;
  int rc = ::stat(tmp_file.c_str(), &st);
  ASSERT_EQ(rc, 0, "stat on real path should succeed");
  ASSERT_NE(st.st_ino, static_cast<ino_t>(0),
            "real file should have non-zero inode");
  ASSERT_NE(st.st_size, static_cast<off_t>(0),
            "real file should have non-zero size");

  // Check this is a regular file
  ASSERT_TRUE(S_ISREG(st.st_mode), "should be a regular file");

  fs::remove_all(tmp_dir);
  PASS();
}

void test_stat_nonexistent_fails() {
  TEST("stat: nonexistent path returns -1");

  struct stat st;
  int rc = ::stat("/tmp/ai_mirror_test_nonexistent_XXXXXX", &st);

  ASSERT_EQ(rc, -1, "stat on nonexistent path should fail");
  PASS();
}

void test_stat_virtual_device_name_fails() {
  TEST("stat: 'beegfs_nodev' virtual device name fails");

  struct stat st;
  int rc = ::stat("beegfs_nodev", &st);

  // Virtual device names are not file paths - stat MUST fail
  ASSERT_EQ(rc, -1, "stat on 'beegfs_nodev' should fail (ENOENT)");
  PASS();
}

// ================================================================
// Test: MountInfo source clearing logic for virtual devices
// ================================================================

void test_clear_virtual_source() {
  TEST("clear: virtual device source should be cleared");

  MountInfo mi;
  mi.source = "beegfs_nodev";

  // Simulate update_state_mounts logic:
  bool is_virtual = is_virtual_source(mi.source);
  if (is_virtual) {
    mi.source.clear(); // <-- the fix
  }

  ASSERT_TRUE(mi.source.empty(),
              "virtual device source should be cleared to empty");
  PASS();
}

void test_keep_real_source() {
  TEST("clear: real path source should be kept");

  MountInfo mi;
  mi.source = "/mnt/beegfs_data/usr/test/file";

  bool is_virtual = is_virtual_source(mi.source);
  if (is_virtual) {
    mi.source.clear();
  }

  ASSERT_FALSE(mi.source.empty(), "real path source should NOT be cleared");
  ASSERT_EQ(mi.source, "/mnt/beegfs_data/usr/test/file",
            "source should be unchanged");
  PASS();
}

// ================================================================
// Test: is_virtual_source in mount_watch health check context
// ================================================================

void test_mount_watch_virtual_skip_logic() {
  TEST("mount_watch: virtual device skips source check");

  // Simulate the health check logic from mount_watch.cpp:264-293
  struct MountEntry {
    std::string source;
    std::string target;
  };

  // Create temp target that exists
  fs::path tmp_dir = fs::temp_directory_path() / "ai_mirror_test_health";
  fs::remove_all(tmp_dir);
  fs::create_directories(tmp_dir);
  fs::path alive_target = tmp_dir / "alive_mount";
  {
    std::ofstream f(alive_target);
    f << "data" << std::endl;
  }
  fs::path dead_target = tmp_dir / "dead_mount";
  // Don't create dead_target - it will be a stale mount

  // Test 1: Virtual device + alive target → NOT stale
  {
    MountEntry virt_alive;
    virt_alive.source = "beegfs_nodev";
    virt_alive.target = alive_target.string();

    bool is_virtual = is_virtual_source(virt_alive.source);
    struct stat st;
    bool target_alive = (::stat(virt_alive.target.c_str(), &st) == 0);

    ASSERT_TRUE(is_virtual, "beegfs_nodev should be virtual");
    ASSERT_TRUE(target_alive, "alive target should stat ok");

    // Health check: virtual devices skip source check
    // Only target check matters
    bool stale = !target_alive;
    ASSERT_FALSE(stale, "virtual + alive target should NOT be stale");
  }

  // Test 2: Virtual device + dead target → stale (target check catches it)
  {
    MountEntry virt_dead;
    virt_dead.source = "beegfs_nodev";
    virt_dead.target = dead_target.string();

    bool is_virtual = is_virtual_source(virt_dead.source);
    struct stat st;
    bool target_alive = (::stat(virt_dead.target.c_str(), &st) == 0);

    ASSERT_TRUE(is_virtual, "beegfs_nodev should be virtual");
    ASSERT_FALSE(target_alive, "dead target should stat fail");

    // Health check: virtual device, target dead → STALE
    bool stale = !target_alive;
    ASSERT_TRUE(stale, "virtual + dead target should be stale");
  }

  // Test 3: Real path + alive target → NOT stale
  {
    MountEntry real_alive;
    real_alive.source = alive_target.string(); // real path
    real_alive.target = alive_target.string();

    bool is_virtual = is_virtual_source(real_alive.source);
    struct stat st;
    bool source_ok = (::stat(real_alive.source.c_str(), &st) == 0);

    ASSERT_FALSE(is_virtual, "real path should NOT be virtual");
    ASSERT_TRUE(source_ok, "real source should stat ok");

    bool stale = false;
    if (!is_virtual && !source_ok) {
      stale = true;
    }
    ASSERT_FALSE(stale, "real path + alive target should NOT be stale");
  }

  // Test 4: Real path + dead target → stale
  {
    MountEntry real_dead;
    real_dead.source = dead_target.string();
    real_dead.target = dead_target.string();

    struct stat st;
    bool target_alive = (::stat(real_dead.target.c_str(), &st) == 0);

    (void)is_virtual_source(real_dead.source); // unused in this branch
    ASSERT_TRUE(!target_alive, "dead target should stat fail");

    bool stale = !target_alive;
    ASSERT_TRUE(stale, "real path + dead target should be stale");
  }

  fs::remove_all(tmp_dir);
  PASS();
}

// ================================================================
// Test: JSON roundtrip of mount info
// ================================================================

void test_json_roundtrip_real() {
  TEST("roundtrip: real path mount");

  // Build MountInfo as update_state_mounts would
  MountInfo mi;
  mi.source = "/tmp/real_file";
  mi.target = "/tmp/mount_target";
  mi.read_only = false;

  // Simulate stat
  struct stat st;
  int rc = ::stat("/tmp", &st);
  if (rc == 0) {
    mi.source_stat.ino = st.st_ino;
    mi.source_stat.dev = st.st_dev;
    mi.source_stat.mode = st.st_mode;
  }

  std::string json = mount_to_json(mi);

  // Verify source_stat is present
  ASSERT_NE(json.find("source_stat"), std::string::npos,
            "real path roundtrip should include source_stat");

  PASS();
}

void test_json_roundtrip_virtual() {
  TEST("roundtrip: virtual device mount");

  MountInfo mi;
  mi.source = "beegfs_nodev"; // will be cleared by update_state_mounts
  mi.source.clear();          // simulating the clear fix
  mi.target = "/tmp/virt_target";
  mi.read_only = true;

  std::string json = mount_to_json(mi);

  // source_stat must NOT be present
  ASSERT_TRUE(json.find("source_stat") == std::string::npos,
              "virtual roundtrip should NOT include source_stat");

  PASS();
}

// ================================================================
// Main
// ================================================================

int main() {
  std::cout << "=== mount_stat unit tests ===" << std::endl;

  // Virtual device detection
  test_virtual_beegfs();
  test_virtual_tmpfs();
  test_virtual_proc();
  test_virtual_empty();
  test_real_path();
  test_real_root();

  // Serialization
  test_serialize_virtual_no_source_stat();
  test_serialize_real_has_source_stat();
  test_serialize_empty_source_virtual();
  test_serialize_real_zero_stat_included();

  // stat(2) system call behavior
  test_stat_real_path();
  test_stat_nonexistent_fails();
  test_stat_virtual_device_name_fails();

  // Source clearing logic
  test_clear_virtual_source();
  test_keep_real_source();

  // Health check logic
  test_mount_watch_virtual_skip_logic();

  // JSON roundtrip
  test_json_roundtrip_real();
  test_json_roundtrip_virtual();

  // Summary
  std::cout << std::endl;
  std::cout << "Results: " << passed << " passed, " << failed << " failed"
            << std::endl;

  return failed > 0 ? 1 : 0;
}

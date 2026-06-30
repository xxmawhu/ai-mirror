// Unit tests for .am_status JSON serialization — verifies that
// nlohmann::ordered_json::dump(2) produces round-trip compatible format
// that read_state_file() can parse, including special characters in path
// fields (the root cause bug).
//
// Compile as standalone binary (no test framework, assert-based).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// PoW (md5 nonce) was removed from production code — no MD5 implementation
// needed.

// ---------------------------------------------------------------------------
// Simulate make_state_content() using ordered_json — PoW removed.
// Matching production code after PoW removal.
// ---------------------------------------------------------------------------
static std::string make_state_content_ordered(const std::string &username,
                                              uid_t uid, gid_t gid,
                                              const std::string &home_dir,
                                              const std::string &main_user,
                                              const std::string &project_path,
                                              const std::string &path_hash) {
  nlohmann::ordered_json j;
  j["username"] = username;
  j["uid"] = uid;
  j["gid"] = gid;
  j["home_dir"] = home_dir;
  j["main_user"] = main_user;
  j["project_path"] = project_path;
  j["path_hash"] = path_hash;
  j["mounts"] = nlohmann::ordered_json::array();
  return j.dump(2) + "\n";
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name, expr)                                                       \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::cerr << "[FAIL] " << name << " (line " << __LINE__ << ")"           \
                << std::endl;                                                  \
      ++fail_count;                                                            \
    } else {                                                                   \
      std::cout << "[PASS] " << name << std::endl;                             \
      ++pass_count;                                                            \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

/// Test 1: Basic round-trip — plain ASCII paths
static void test_basic_roundtrip() {
  std::string username = "imaxx_abc123";
  uid_t uid = 10020001;
  gid_t gid = 10020001;
  std::string home_dir = "/home/imaxx_abc123";
  std::string main_user = "maxx";
  std::string project_path = "/mnt/beegfs_data/usr/maxx/my-project";
  std::string path_hash = "abc123";

  std::string content = make_state_content_ordered(
      username, uid, gid, home_dir, main_user, project_path, path_hash);

  // Verify JSON is parseable
  auto j = nlohmann::json::parse(content);
  TEST("JSON parseable", !j.is_discarded());

  // Verify all fields
  TEST("username matches", j["username"] == username);
  TEST("uid matches", j["uid"] == uid);
  TEST("gid matches", j["gid"] == gid);
  TEST("home_dir matches", j["home_dir"] == home_dir);
  TEST("main_user matches", j["main_user"] == main_user);
  TEST("project_path matches", j["project_path"] == project_path);
  TEST("path_hash matches", j["path_hash"] == path_hash);
  TEST("no timestamp (PoW removed)", !j.contains("timestamp"));
  TEST("mounts is array", j["mounts"].is_array());
  TEST("mounts empty", j["mounts"].empty());
}

/// Test 2: Special characters in paths — the root cause bug
static void test_special_chars_in_paths() {
  // Path with characters that would break hand-concatenated JSON:
  // double quote and backslash
  std::string evil_path =
      "/mnt/beegfs_data/usr/maxx/quant/model/dev-\"quoted\"/path";
  std::string evil_home = "/home/imaxx_evil/back\\slash";
  std::string username = "imaxx_evil01";
  uid_t uid = 10020002;
  gid_t gid = 10020002;
  std::string main_user = "maxx";
  std::string path_hash = "def456";

  std::string content = make_state_content_ordered(
      username, uid, gid, evil_home, main_user, evil_path, path_hash);

  // Verify JSON is parseable (this is the key test — old code would fail here)
  auto j = nlohmann::json::parse(content);
  TEST("(special) JSON parseable with quotes and backslashes",
       !j.is_discarded());

  // Verify read-back values
  TEST("(special) project_path matches", j["project_path"] == evil_path);
  TEST("(special) home_dir matches", j["home_dir"] == evil_home);
  TEST("(special) username matches", j["username"] == username);
}

/// Test 3: Output format — verify dump(2) produces valid JSON with newlines
static void test_output_format() {
  std::string content = make_state_content_ordered(
      "imaxx_fmt01", 10020003, 10020003, "/home/imaxx_fmt01", "maxx",
      "/project/fmt-test", "fmt001");

  // Must end with newline
  TEST("trailing newline", content.back() == '\n');

  // Must start with '{'
  TEST("starts with {", content.front() == '{');

  // Must contain 2-space indented fields
  TEST("2-space indent", content.find("  \"username\"") != std::string::npos);

  // Verify it's valid JSON
  auto j = nlohmann::json::parse(content);
  TEST("(format) JSON valid", !j.is_discarded());

  // Verify ordered field preservation: username should be first field
  size_t pos_username = content.find("\"username\"");
  size_t pos_project = content.find("\"project_path\"");
  TEST("field order: username before project_path", pos_username < pos_project);
}

/// Test 4: Backward compatibility — old format (no project_path/path_hash)
static void test_backward_compat_old_format() {
  std::string old_content = "{\n"
                            "  \"username\": \"imaxx_legacy\",\n"
                            "  \"uid\": 10020004,\n"
                            "  \"gid\": 10020004,\n"
                            "  \"home_dir\": \"/home/imaxx_legacy\",\n"
                            "  \"main_user\": \"maxx\"\n"
                            "}\n";

  auto j = nlohmann::json::parse(old_content);
  TEST("(backward) old format parseable", !j.is_discarded());
  TEST("(backward) username", j["username"] == "imaxx_legacy");
  TEST("(backward) no project_path (legacy)", !j.contains("project_path"));
  TEST("(backward) no path_hash (legacy)", !j.contains("path_hash"));
}

/// Test 5: Legacy hash format — verify it's still parseable
static void test_backward_compat_hash_format() {
  std::string legacy_content = "{\n"
                               "  \"username\": \"imaxx_hashfmt\",\n"
                               "  \"uid\": 10020005,\n"
                               "  \"gid\": 10020005,\n"
                               "  \"home_dir\": \"/home/imaxx_hashfmt\",\n"
                               "  \"main_user\": \"maxx\",\n"
                               "  \"hash\": \"000def789\",\n"
                               "  \"timestamp\": 1234567890\n"
                               "}\n";

  auto j = nlohmann::json::parse(legacy_content);
  TEST("(hashfmt) legacy hash parseable", !j.is_discarded());
  TEST("(hashfmt) hash field present", j.contains("hash"));
}

/// Test 6: New format with mounts array
static void test_with_mounts() {
  nlohmann::ordered_json j;
  j["username"] = "imaxx_mount1";
  j["uid"] = 10020006;
  j["gid"] = 10020006;
  j["home_dir"] = "/home/imaxx_mount1";
  j["main_user"] = "maxx";
  j["project_path"] = "/project/with-mounts";
  j["path_hash"] = "mount01";

  nlohmann::ordered_json mounts = nlohmann::ordered_json::array();
  nlohmann::ordered_json m1;
  m1["source"] = "/data/src";
  m1["target"] = "/mnt/target";
  m1["read_only"] = false;
  m1["source_stat"] = {{"ino", 12345},       {"dev", 2049}, {"mode", 0755},
                       {"uid", 1000},        {"gid", 1000}, {"size", 4096},
                       {"mtime", 1700000000}};
  mounts.push_back(m1);

  nlohmann::ordered_json m2;
  m2["source"] = "/data/readonly";
  m2["target"] = "/mnt/ro";
  m2["read_only"] = true;
  mounts.push_back(m2);

  j["mounts"] = mounts;
  j["timestamp"] = 1234567890;

  std::string content = j.dump(2) + "\n";

  auto j2 = nlohmann::json::parse(content);
  TEST("(mounts) JSON valid", !j2.is_discarded());
  TEST("(mounts) mounts is array", j2["mounts"].is_array());
  TEST("(mounts) 2 mount entries", j2["mounts"].size() == 2);
  TEST("(mounts) first source", j2["mounts"][0]["source"] == "/data/src");
  TEST("(mounts) first read_only false", j2["mounts"][0]["read_only"] == false);
  TEST("(mounts) second read_only true", j2["mounts"][1]["read_only"] == true);
  TEST("(mounts) source_stat in first",
       j2["mounts"][0].contains("source_stat"));
  TEST("(mounts) no source_stat in second",
       !j2["mounts"][1].contains("source_stat"));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
  std::cout << "=== State JSON Serialization Tests ===" << std::endl;
  test_basic_roundtrip();
  test_special_chars_in_paths();
  test_output_format();
  test_backward_compat_old_format();
  test_backward_compat_hash_format();
  test_with_mounts();

  std::cout << "\n=== Results: " << pass_count << " passed, " << fail_count
            << " failed ===" << std::endl;
  return fail_count > 0 ? 1 : 0;
}

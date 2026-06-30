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

// ---------------------------------------------------------------------------
// Minimal MD5 implementation (no OpenSSL dependency in test)
// ---------------------------------------------------------------------------
namespace {

// Left-rotate a 32-bit value by n bits
inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// MD5 round constants
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

// Shift amounts per round
static const uint32_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

// Compute MD5 hash and return as hex string
static std::string md5_hex(const uint8_t *input, size_t len) {
  // Padding
  size_t padded_len = ((len + 8 + 64) / 64) * 64;
  std::vector<uint8_t> buf(padded_len, 0);
  std::copy(input, input + len, buf.begin());
  buf[len] = 0x80;

  // Append length in bits (little-endian)
  uint64_t bit_len = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i) {
    buf[padded_len - 8 + i] = static_cast<uint8_t>(bit_len >> (i * 8));
  }

  uint32_t h0 = 0x67452301, h1 = 0xefcdab89;
  uint32_t h2 = 0x98badcfe, h3 = 0x10325476;

  for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
    uint32_t w[16];
    for (int i = 0; i < 16; ++i) {
      w[i] = static_cast<uint32_t>(buf[chunk + i * 4]) |
             (static_cast<uint32_t>(buf[chunk + i * 4 + 1]) << 8) |
             (static_cast<uint32_t>(buf[chunk + i * 4 + 2]) << 16) |
             (static_cast<uint32_t>(buf[chunk + i * 4 + 3]) << 24);
    }

    uint32_t a = h0, b = h1, c = h2, d = h3;
    for (int i = 0; i < 64; ++i) {
      uint32_t f, g;
      if (i < 16) {
        f = (b & c) | ((~b) & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | ((~d) & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | (~d));
        g = (7 * i) % 16;
      }
      f += a + K[i] + w[g];
      a = d;
      d = c;
      c = b;
      b += rotl32(f, S[i]);
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
  }

  std::ostringstream oss;
  auto wr = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i)
      oss << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<int>((v >> (i * 8)) & 0xff);
  };
  wr(h0);
  wr(h1);
  wr(h2);
  wr(h3);
  return oss.str();
}

static std::string md5_hex(const std::string &input) {
  return md5_hex(reinterpret_cast<const uint8_t *>(input.data()), input.size());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Simulate make_state_content() using ordered_json — the new implementation.
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

  auto now = std::chrono::system_clock::now();
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count();

  // PoW: find a timestamp that makes md5(content) start with "000"
  for (int64_t t = us;; ++t) {
    j["timestamp"] = t;
    std::string content = j.dump(2) + "\n";
    if (md5_hex(content).substr(0, 3) == "000") {
      return content;
    }
  }
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

  // Verify PoW
  TEST("PoW valid (starts with 000)", md5_hex(content).substr(0, 3) == "000");

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
  TEST("timestamp is number", j["timestamp"].is_number_integer());
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

  // Verify PoW
  TEST("(special) PoW valid", md5_hex(content).substr(0, 3) == "000");

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

/// Test 4: Backward compatibility — old format (pre-PoW, no hash)
static void test_backward_compat_old_format() {
  std::string old_content = "{\n"
                            "  \"username\": \"imaxx_legacy\",\n"
                            "  \"uid\": 10020004,\n"
                            "  \"gid\": 10020004,\n"
                            "  \"home_dir\": \"/home/imaxx_legacy\",\n"
                            "  \"main_user\": \"maxx\",\n"
                            "  \"timestamp\": 1234567890\n"
                            "}\n";

  auto j = nlohmann::json::parse(old_content);
  TEST("(backward) old format parseable", !j.is_discarded());
  TEST("(backward) username", j["username"] == "imaxx_legacy");
  TEST("(backward) no project_path (legacy)", !j.contains("project_path"));
  TEST("(backward) no path_hash (legacy)", !j.contains("path_hash"));
}

/// Test 5: Legacy hash format
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
  TEST("(hashfmt) hash starts with 000",
       j["hash"].get<std::string>().substr(0, 3) == "000");
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

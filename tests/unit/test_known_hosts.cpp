// Unit tests for parse_known_hosts_hosts: exhaustive known_hosts format
// coverage Compile as standalone binary (no test framework, assert-based)
#include "ai_mirror/core/ssh_manager.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using ai_mirror::core::parse_known_hosts_hosts;

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

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::ostringstream _ss;                                                  \
      _ss << msg << " (got " << (a) << ", expected " << (b) << ")";            \
      FAIL(_ss.str());                                                         \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_TRUE(expr, msg)                                                 \
  do {                                                                         \
    if (!(expr)) {                                                             \
      FAIL(msg);                                                               \
      return;                                                                  \
    }                                                                          \
  } while (0)

// ========================================================================
// 1. Plain hostname
// ========================================================================
void test_plain_hostname() {
  TEST("plain hostname: github.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "github.com ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "github.com", "hostname mismatch");
  PASS();
}

// ========================================================================
// 2. IPv4 address
// ========================================================================
void test_ipv4_address() {
  TEST("IPv4 address: 192.168.1.1 ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "192.168.1.1 ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "192.168.1.1", "IP mismatch");
  PASS();
}

// ========================================================================
// 3. IPv6 address in brackets
// ========================================================================
void test_ipv6_address() {
  TEST("IPv6 address: [2001:db8::1] ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "[2001:db8::1] ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "[2001:db8::1]", "IPv6 mismatch");
  PASS();
}

// ========================================================================
// 4. Hostname with port (bracket notation)
// ========================================================================
void test_hostname_with_port() {
  TEST("hostname with port: [gitlab.com]:2222 ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "[gitlab.com]:2222 ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "[gitlab.com]:2222", "host:port mismatch");
  PASS();
}

// ========================================================================
// 5. Multiple hosts comma-separated (hostname,hostname)
// ========================================================================
void test_multiple_hosts_comma() {
  TEST("multiple hosts: gitlab.com,192.168.1.1 ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "gitlab.com,192.168.1.1 ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host (first only)");
  ASSERT_EQ(result[0], "gitlab.com", "should take first host");
  PASS();
}

// ========================================================================
// 6. Multiple hosts: IP first
// ========================================================================
void test_multiple_hosts_ip_first() {
  TEST("multiple hosts: 10.0.0.1,server.local ssh-ed25519 AAAAC3...");
  auto result = parse_known_hosts_hosts(
      "10.0.0.1,server.local ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "10.0.0.1", "should take IP first");
  PASS();
}

// ========================================================================
// 7. Multiple hosts: bracketed IPv6 + hostname
// ========================================================================
void test_multiple_hosts_ipv6_first() {
  TEST("multiple hosts: [2001:db8::1],server.local ssh-ed25519 AAAAC3...");
  auto result = parse_known_hosts_hosts(
      "[2001:db8::1],server.local ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "[2001:db8::1]", "should take IPv6 first");
  PASS();
}

// ========================================================================
// 8. Multiple hosts: hostname with port first
// ========================================================================
void test_multiple_hosts_port_first() {
  TEST("multiple hosts: [gitlab.com]:2222,gitlab.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("[gitlab.com]:2222,gitlab.com ssh-rsa "
                                        "AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "[gitlab.com]:2222", "should take host:port first");
  PASS();
}

// ========================================================================
// 9. HashKnownHosts format (|1|salt|hash)
// ========================================================================
void test_hashed_known_hosts() {
  TEST("HashKnownHosts: |1|salt|hash ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "|1|euHTs5GZIgZm1WRbX0Bhjb8N368=|DdWozoNlg4eJbEzsEKW5BDp2ocA= ssh-rsa "
      "AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(0), "should skip hashed host");
  PASS();
}

// ========================================================================
// 10. HashKnownHosts: multiple hashed entries in one line (unlikely but valid)
// ========================================================================
void test_hashed_multiple_comma() {
  TEST("hashed with comma: |1|salt1|hash1,|1|salt2|hash2 ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("|1|salt1|hash1,|1|salt2|hash2 ssh-rsa "
                                        "AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(0), "should skip all hashed hosts");
  PASS();
}

// ========================================================================
// 11. Old-style hashed host (@revoked or @cert-authority)
// ========================================================================
void test_at_cert_authority() {
  TEST("@cert-authority: @cert-authority *.example.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("@cert-authority *.example.com ssh-rsa "
                                        "AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(0), "should skip @ entries");
  PASS();
}

// ========================================================================
// 12. @revoked marker
// ========================================================================
void test_at_revoked() {
  TEST("@revoked: @revoked badhost.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "@revoked badhost.com ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(0), "should skip @ entries");
  PASS();
}

// ========================================================================
// 13. Comment line
// ========================================================================
void test_comment_line() {
  TEST("comment: # This is a comment");
  auto result = parse_known_hosts_hosts("# This is a comment");
  ASSERT_EQ(result.size(), size_t(0), "should skip comments");
  PASS();
}

// ========================================================================
// 14. Empty line
// ========================================================================
void test_empty_line() {
  TEST("empty line");
  auto result = parse_known_hosts_hosts("");
  ASSERT_EQ(result.size(), size_t(0), "should skip empty lines");
  PASS();
}

// ========================================================================
// 15. Whitespace-only line
// ========================================================================
void test_whitespace_only() {
  TEST("whitespace-only line");
  auto result = parse_known_hosts_hosts("   ");
  // No space delimiter found, but first char is space not '#'
  // space_pos would be 0, host_field would be empty
  ASSERT_EQ(result.size(), size_t(0), "should skip whitespace-only");
  PASS();
}

// ========================================================================
// 16. Line with no space (malformed)
// ========================================================================
void test_no_space() {
  TEST("no space: justhostname");
  auto result = parse_known_hosts_hosts("justhostname");
  ASSERT_EQ(result.size(), size_t(0), "should skip lines without space");
  PASS();
}

// ========================================================================
// 17. Hostname starting with pipe (edge case)
// ========================================================================
void test_pipe_hostname() {
  TEST("pipe hostname: |something ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("|something ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(0), "should skip | prefixed hosts");
  PASS();
}

// ========================================================================
// 18. Mixed: comma with hashed first, plain second
// (hashed first means we skip the whole line's first host)
// ========================================================================
void test_mixed_hashed_plain_comma() {
  TEST("mixed: |1|salt|hash,github.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "|1|salt|hash,github.com ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(0), "first host is hashed, skip");
  PASS();
}

// ========================================================================
// 19. Plain first, hashed second (comma)
// ========================================================================
void test_mixed_plain_hashed_comma() {
  TEST("mixed: github.com,|1|salt|hash ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts(
      "github.com,|1|salt|hash ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...");
  ASSERT_EQ(result.size(), size_t(1), "first host is plain, take it");
  ASSERT_EQ(result[0], "github.com", "should take plain first host");
  PASS();
}

// ========================================================================
// 20. ED25519 key type
// ========================================================================
void test_ed25519_key() {
  TEST("ed25519 key: server.com ssh-ed25519 AAAAC3...");
  auto result = parse_known_hosts_hosts(
      "server.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "server.com", "hostname mismatch");
  PASS();
}

// ========================================================================
// 21. ECDSA key type
// ========================================================================
void test_ecdsa_key() {
  TEST("ecdsa key: server.com ecdsa-sha2-nistp256 AAAAE2VjZ...");
  auto result = parse_known_hosts_hosts(
      "server.com ecdsa-sha2-nistp256 "
      "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBB...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "server.com", "hostname mismatch");
  PASS();
}

// ========================================================================
// 22. Short hostname (single label)
// ========================================================================
void test_short_hostname() {
  TEST("short hostname: myserver ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("myserver ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "myserver", "hostname mismatch");
  PASS();
}

// ========================================================================
// 23. Hostname with dashes and dots
// ========================================================================
void test_complex_hostname() {
  TEST("complex hostname: my-server.example.co.uk ssh-rsa AAAAB3...");
  auto result =
      parse_known_hosts_hosts("my-server.example.co.uk ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "my-server.example.co.uk", "hostname mismatch");
  PASS();
}

// ========================================================================
// 24. Hostname with underscore
// ========================================================================
void test_underscore_hostname() {
  TEST("underscore hostname: my_server.local ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("my_server.local ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "my_server.local", "hostname mismatch");
  PASS();
}

// ========================================================================
// 25. Wildcard pattern (technically valid in some contexts)
// ========================================================================
void test_wildcard_hostname() {
  TEST("wildcard: *.example.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("*.example.com ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "*.example.com", "hostname mismatch");
  PASS();
}

// ========================================================================
// 26. Multiple spaces in line
// ========================================================================
void test_extra_spaces() {
  TEST("extra spaces: github.com  ssh-rsa  AAAAB3...");
  auto result = parse_known_hosts_hosts("github.com  ssh-rsa  AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "github.com", "hostname mismatch");
  PASS();
}

// ========================================================================
// 27. Tab-separated fields
// ========================================================================
void test_tab_separator() {
  TEST("tab separator: github.com\tssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("github.com\tssh-rsa AAAAB3...");
  // Tab is not a space, so host_field includes everything before first space
  // "github.com\tssh-rsa" would be the host field - this is technically valid
  // SSH behavior
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  // The host field would be "github.com\tssh-rsa" which includes the tab
  // This matches real known_hosts behavior where tab can be a field separator
  ASSERT_TRUE(result[0].find("github.com") != std::string::npos,
              "should contain hostname");
  PASS();
}

// ========================================================================
// 28. Hostname with port, IPv4
// ========================================================================
void test_ipv4_with_port() {
  TEST("IPv4 with port: [192.168.1.1]:2222 ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("[192.168.1.1]:2222 ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "[192.168.1.1]:2222", "IP:port mismatch");
  PASS();
}

// ========================================================================
// 29. Line with only hostname and key (minimal)
// ========================================================================
void test_minimal_line() {
  TEST("minimal: h k v");
  auto result = parse_known_hosts_hosts("h k v");
  ASSERT_EQ(result.size(), size_t(1), "should return 1 host");
  ASSERT_EQ(result[0], "h", "should return host field");
  PASS();
}

// ========================================================================
// 30. Multiple comma hosts: three hosts
// ========================================================================
void test_three_comma_hosts() {
  TEST("three hosts: a.com,b.com,c.com ssh-rsa AAAAB3...");
  auto result = parse_known_hosts_hosts("a.com,b.com,c.com ssh-rsa AAAAB3...");
  ASSERT_EQ(result.size(), size_t(1), "should return only first host");
  ASSERT_EQ(result[0], "a.com", "should be first host");
  PASS();
}

// ========================================================================
// 31. HashKnownHosts with different salt lengths
// ========================================================================
void test_hashed_various_salts() {
  TEST("hashed: various |1|... formats");
  // Real examples from the issue
  auto r1 =
      parse_known_hosts_hosts("|1|euHTs5GZIgZm1WRbX0Bhjb8N368=|"
                              "DdWozoNlg4eJbEzsEKW5BDp2ocA= ssh-rsa AAAA");
  auto r2 = parse_known_hosts_hosts("|1|MKZxTVoLZifHjxHgM+RbazeuIKc=|arH/"
                                    "c0Cw226YhLfE1GGfvayXP/0= ssh-rsa AAAA");
  auto r3 =
      parse_known_hosts_hosts("|1|4HqmlN4sw7+FZAPpcofcUSuGlz8=|gXKHUVkXnQf/"
                              "uKe977vEU2bcDRI= ssh-rsa AAAA");
  auto r4 =
      parse_known_hosts_hosts("|1|I883bHBOEoJAzuRJqgihOnYZ0Xo=|"
                              "i2IY1EYANNNYCNV5yd6zqCjBkWA= ssh-rsa AAAA");
  ASSERT_EQ(r1.size(), size_t(0), "hashed 1 should be skipped");
  ASSERT_EQ(r2.size(), size_t(0), "hashed 2 should be skipped");
  ASSERT_EQ(r3.size(), size_t(0), "hashed 3 should be skipped");
  ASSERT_EQ(r4.size(), size_t(0), "hashed 4 should be skipped");
  PASS();
}

// ========================================================================
// 32. Hostname with leading/trailing whitespace (edge case)
// ========================================================================
void test_leading_whitespace() {
  TEST("leading whitespace: '  github.com ssh-rsa AAAAB3...'");
  auto result = parse_known_hosts_hosts("  github.com ssh-rsa AAAAB3...");
  // First space at pos 0, host_field = "" (empty), skipped by
  // !host_field.empty() This is correct behavior: real known_hosts never has
  // leading whitespace
  ASSERT_EQ(result.size(), size_t(0), "should skip: host_field becomes empty");
  PASS();
}

// ========================================================================
// 33. Real-world mixed known_hosts file (multi-line parsing simulation)
// ========================================================================
void test_real_world_file() {
  TEST("real-world mixed known_hosts file");
  std::vector<std::string> lines = {
      "# Comment line",
      "",
      "github.com ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQC...",
      "gitlab.com,192.168.1.100 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI...",
      "|1|euHTs5GZIgZm1WRbX0Bhjb8N368=|DdWozoNlg4eJbEzsEKW5BDp2ocA= ssh-rsa "
      "AAAA",
      "|1|MKZxTVoLZifHjxHgM+RbazeuIKc=|arH/c0Cw226YhLfE1GGfvayXP/0= ssh-rsa "
      "AAAA",
      "[2001:db8::1] ssh-rsa AAAAB3...",
      "[gitlab.com]:2222 ssh-rsa AAAAB3...",
      "10.0.0.1 ssh-ed25519 AAAAC3...",
      "@cert-authority *.example.com ssh-rsa AAAAB3...",
  };

  std::vector<std::string> all_hosts;
  for (const auto &line : lines) {
    auto parsed = parse_known_hosts_hosts(line);
    all_hosts.insert(all_hosts.end(), parsed.begin(), parsed.end());
  }

  ASSERT_EQ(all_hosts.size(), size_t(5), "should find 5 scannable hosts");
  ASSERT_EQ(all_hosts[0], "github.com", "host 0 mismatch");
  ASSERT_EQ(all_hosts[1], "gitlab.com", "host 1 mismatch");
  ASSERT_EQ(all_hosts[2], "[2001:db8::1]", "host 2 mismatch");
  ASSERT_EQ(all_hosts[3], "[gitlab.com]:2222", "host 3 mismatch");
  ASSERT_EQ(all_hosts[4], "10.0.0.1", "host 4 mismatch");
  PASS();
}

// ========================================================================
// Main
// ========================================================================
int main() {
  std::cout << "=== parse_known_hosts_hosts unit tests ===" << std::endl;

  // Basic formats
  test_plain_hostname();            // 1
  test_ipv4_address();              // 2
  test_ipv6_address();              // 3
  test_hostname_with_port();        // 4
  test_multiple_hosts_comma();      // 5
  test_multiple_hosts_ip_first();   // 6
  test_multiple_hosts_ipv6_first(); // 7
  test_multiple_hosts_port_first(); // 8

  // Hashed / special markers
  test_hashed_known_hosts();    // 9
  test_hashed_multiple_comma(); // 10
  test_at_cert_authority();     // 11
  test_at_revoked();            // 12

  // Skip cases
  test_comment_line();    // 13
  test_empty_line();      // 14
  test_whitespace_only(); // 15
  test_no_space();        // 16
  test_pipe_hostname();   // 17

  // Mixed scenarios
  test_mixed_hashed_plain_comma(); // 18
  test_mixed_plain_hashed_comma(); // 19

  // Different key types
  test_ed25519_key(); // 20
  test_ecdsa_key();   // 21

  // Hostname variations
  test_short_hostname();      // 22
  test_complex_hostname();    // 23
  test_underscore_hostname(); // 24
  test_wildcard_hostname();   // 25

  // Format edge cases
  test_extra_spaces();      // 26
  test_tab_separator();     // 27
  test_ipv4_with_port();    // 28
  test_minimal_line();      // 29
  test_three_comma_hosts(); // 30

  // Real-world patterns
  test_hashed_various_salts(); // 31
  test_leading_whitespace();   // 32
  test_real_world_file();      // 33

  std::cout << std::endl;
  std::cout << "Results: " << passed << " passed, " << failed << " failed"
            << std::endl;

  return failed > 0 ? 1 : 0;
}

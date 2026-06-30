// test_popen.cpp - Test popen() execution of am mv command
// This demonstrates the issue: popen() uses /bin/sh -c which doesn't support
// bash functions
//
// Compile: g++ -o test_popen test_popen.cpp
// Run: ./test_popen

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// Simulate popen() behavior
std::string execute_popen_sh(const std::string &cmd) {
  std::cout << "[test] Testing popen with /bin/sh (default): " << cmd
            << std::endl;

  // popen() internally does: /bin/sh -c "cmd"
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) {
    return "popen failed";
  }

  std::string result;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), fp)) {
    result += buffer;
  }
  int status = pclose(fp);

  result += "\n[exit code: " + std::to_string(status) + "]";
  return result;
}

std::string execute_popen_bash(const std::string &cmd) {
  std::cout << "[test] Testing popen with bash (explicit): " << cmd
            << std::endl;

  // Alternative: use bash explicitly
  std::string bash_cmd = "bash -c '" + cmd + "'";
  FILE *fp = popen(bash_cmd.c_str(), "r");
  if (!fp) {
    return "popen failed";
  }

  std::string result;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), fp)) {
    result += buffer;
  }
  int status = pclose(fp);

  result += "\n[exit code: " + std::to_string(status) + "]";
  return result;
}

std::string execute_direct_binary(const std::string &cmd) {
  std::cout << "[test] Testing direct binary call (recommended): " << cmd
            << std::endl;

  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) {
    return "popen failed";
  }

  std::string result;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), fp)) {
    result += buffer;
  }
  int status = pclose(fp);

  result += "\n[exit code: " + std::to_string(status) + "]";
  return result;
}

int main() {
  std::cout << "=== popen() am mv Test ===" << std::endl;
  std::cout << std::endl;

  // Test 1: popen() default (sh -c) - This will FAIL
  std::cout << "=== Test 1: popen() default (/bin/sh -c) ===" << std::endl;
  std::cout
      << "ROOT CAUSE: popen() uses /bin/sh which doesn't support bash functions"
      << std::endl;
  std::cout << std::endl;

  std::string cmd1 =
      "source /etc/profile.d/am.sh && am mv /tmp/test.md /tmp/dst/";
  std::string result1 = execute_popen_sh(cmd1);
  std::cout << "Result:\n" << result1 << std::endl;

  if (result1.find("not found") != std::string::npos ||
      result1.find("syntax error") != std::string::npos ||
      result1.find("am:") != std::string::npos) {
    std::cout << "[expected] sh doesn't support bash functions" << std::endl;
  }

  std::cout << std::endl;

  // Test 2: popen() with bash -c - This should work
  std::cout << "=== Test 2: popen() with bash -c ===" << std::endl;
  std::string cmd2 =
      "source /etc/profile.d/am.sh && am mv /tmp/test.md /tmp/dst/";
  std::string result2 = execute_popen_bash(cmd2);
  std::cout << "Result:\n" << result2 << std::endl;

  std::cout << std::endl;

  // Test 3: Direct binary call - The recommended solution
  std::cout << "=== Test 3: Direct binary call (recommended solution) ==="
            << std::endl;
  std::cout << "SOLUTION: Call /usr/local/bin/ai-mirror-bin directly, not "
               "through am function"
            << std::endl;
  std::cout << std::endl;

  std::string cmd3 = "/usr/local/bin/ai-mirror-bin mv /tmp/test.md /tmp/dst/";
  std::string result3 = execute_direct_binary(cmd3);
  std::cout << "Result:\n" << result3 << std::endl;

  std::cout << std::endl;
  std::cout << "=== Summary ===" << std::endl;
  std::cout << std::endl;
  std::cout << "PROBLEM:" << std::endl;
  std::cout << "  1. popen() uses /bin/sh -c by default" << std::endl;
  std::cout << "  2. sh doesn't support bash functions (am is a bash function)"
            << std::endl;
  std::cout << "  3. Even with bash, source in subshell doesn't affect caller"
            << std::endl;
  std::cout << std::endl;
  std::cout << "SOLUTION:" << std::endl;
  std::cout
      << "  1. Call /usr/local/bin/ai-mirror-bin directly (not am function)"
      << std::endl;
  std::cout << "  2. Or use: bash -c 'source /etc/profile.d/am.sh && am ...'"
            << std::endl;
  std::cout << "  3. Handle sudo yourself (am.sh adds sudo for non-root users)"
            << std::endl;
  std::cout << std::endl;

  return 0;
}

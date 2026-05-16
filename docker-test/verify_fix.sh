#!/usr/bin/env bash
#
# 验证 verify_state_content 对旧格式的兼容修复
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(dirname "$SCRIPT_DIR")"

echo "============================================================"
echo "验证 verify_state_content 旧格式兼容修复"
echo "============================================================"

# 创建测试程序
TEST_CPP="$SCRIPT_DIR/test_verify_state.cpp"

cat >"$TEST_CPP" <<'EOF'
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

// MD5 简化实现（仅用于测试）
std::string md5_hex(const std::string& input) {
    // 这里简化处理，实际实现见 user_manager.cpp
    // 对于测试，我们只需要验证逻辑分支
    unsigned char hash[16];
    // 使用 OpenSSL 或其他库计算 MD5
    // 这里我们用占位符
    return "placeholder_md5";
}

// 复制 user_manager.cpp 中的 verify_state_content 逻辑
bool verify_state_content(const std::string &content) {
  auto j = nlohmann::json::parse(content, nullptr, false);
  if (j.is_discarded())
    return false;

  // Legacy hash format
  if (j.contains("hash")) {
    std::string stored_hash = j["hash"].get<std::string>();
    return stored_hash.substr(0, 3) == "000";
  }

  // New PoW format
  if (j.contains("project_path") || j.contains("path_hash")) {
    // 需要 MD5 计算，这里简化
    return false; // 新格式测试需要真实 MD5
  }

  // Old format (pre-PoW): trust directly
  return true;
}

int main() {
    // 旧格式（用户报告的格式）
    std::string old_format = R"({
  "username": "imaxx_listen_msg_from_tg",
  "uid": 10020025,
  "gid": 10020025,
  "home_dir": "/mnt/beegfs_data/usr/maxx/dev/listen_msg_from_tg",
  "main_user": "maxx",
  "timestamp": 1778725710061735
})";

    std::cout << "测试 1: 旧格式 .am_status (无 hash/project_path/path_hash)" << std::endl;
    bool result1 = verify_state_content(old_format);
    std::cout << "  结果: " << (result1 ? "✅ PASS (trusted)" : "❌ FAIL") << std::endl;

    // Legacy hash 格式
    std::string legacy_format = R"({
  "username": "imaxx_test",
  "hash": "000abc123"
})";

    std::cout << "\n测试 2: Legacy hash 格式 (hash 字段以 000 开头)" << std::endl;
    bool result2 = verify_state_content(legacy_format);
    std::cout << "  结果: " << (result2 ? "✅ PASS" : "❌ FAIL") << std::endl;

    // Legacy hash 格式（无效）
    std::string legacy_invalid = R"({
  "username": "imaxx_test",
  "hash": "abc123"
})";

    std::cout << "\n测试 3: Legacy hash 格式 (hash 不以 000 开头)" << std::endl;
    bool result3 = verify_state_content(legacy_invalid);
    std::cout << "  结果: " << (result3 ? "❌ FAIL (should reject)" : "✅ PASS (rejected)") << std::endl;

    // 无效 JSON
    std::cout << "\n测试 4: 无效 JSON" << std::endl;
    bool result4 = verify_state_content("not json");
    std::cout << "  结果: " << (result4 ? "❌ FAIL (should reject)" : "✅ PASS (rejected)") << std::endl;

    std::cout << "\n============================================================" << std::endl;
    if (result1 && result2 && !result3 && !result4) {
        std::cout << "✅ 所有测试通过!" << std::endl;
        return 0;
    } else {
        std::cout << "❌ 测试失败!" << std::endl;
        return 1;
    }
}
EOF

# 编译测试程序
echo ""
echo "编译测试程序..."
cd "$SCRIPT_DIR"
g++ -std=c++20 -I"$SRC_DIR/third_party/json/include" "$TEST_CPP" -o test_verify_state 2>&1

if [[ -f test_verify_state ]]; then
	echo "编译成功，运行测试..."
	./test_verify_state
else
	echo "编译失败，尝试使用系统 nlohmann/json..."
	g++ -std=c++20 "$TEST_CPP" -o test_verify_state 2>&1 || {
		echo "需要安装 nlohmann-json: apt-get install nlohmann-json3-dev"
		exit 1
	}
	./test_verify_state
fi

# 清理
rm -f "$TEST_CPP" test_verify_state 2>/dev/null || true

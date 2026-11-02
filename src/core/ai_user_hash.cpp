// DESIGN BACKGROUND
// AI 用户 home 目录名需要由项目路径稳定派生，且不同项目之间冲突概率可忽略。
// DESIGN RATIONALE
// 取 SHA-256 摘要按大端整数做 base36 编码并截断 12 位，字符集仅 [0-9a-z]，
// 可直接用于 Linux 用户名与目录名，无需转义。
// EVIDENCE
// 12 位 base36 约 62 bit，远高于旧布局 6 位十六进制的 24 bit；
// tests/unit/test_ai_user_home.cpp 覆盖格式、确定性与字符集校验。

#include "ai_mirror/core/ai_user_hash.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include <openssl/evp.h>

namespace ai_mirror::core {
namespace {

constexpr std::size_t kSha256Bytes = 32;

std::array<unsigned char, kSha256Bytes> sha256_digest(std::string_view input,
                                                      bool &ok) {
  std::array<unsigned char, kSha256Bytes> digest{};
  unsigned int digest_len = 0;
  const char *data = input.empty() ? "" : input.data();
  ok = EVP_Digest(data, input.size(), digest.data(), &digest_len, EVP_sha256(),
                  nullptr) == 1 &&
       digest_len == kSha256Bytes;
  return digest;
}

} // namespace

std::string make_ai_user_hash(std::string_view seed) {
  bool digest_ok = false;
  const auto digest = sha256_digest(seed, digest_ok);
  if (!digest_ok) {
    return {};
  }

  // 每轮按 36 取余并整除，剩余数值严格递减；高位零字节被移除，
  // 因此循环在至多 kSha256Bytes * 8 / log2(36) 轮后必然终止。
  std::vector<unsigned char> bytes(digest.begin(), digest.end());
  std::string reversed;
  reversed.reserve(52);
  while (!bytes.empty()) {
    unsigned int remainder = 0;
    for (auto &byte : bytes) {
      const unsigned int current = remainder * 256u + byte;
      byte = static_cast<unsigned char>(current / 36u);
      remainder = current % 36u;
    }
    reversed.push_back(kAiUserHashAlphabet[remainder]);
    while (!bytes.empty() && bytes.front() == 0) {
      bytes.erase(bytes.begin());
    }
  }

  std::reverse(reversed.begin(), reversed.end());
  if (reversed.size() < kAiUserHashLength) {
    reversed.insert(0, kAiUserHashLength - reversed.size(), '0');
  }
  return reversed.substr(0, kAiUserHashLength);
}

bool is_valid_ai_user_hash(std::string_view value) {
  if (value.size() != kAiUserHashLength) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char c) {
    return kAiUserHashAlphabet.find(c) != std::string_view::npos;
  });
}

} // namespace ai_mirror::core

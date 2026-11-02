#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ai_mirror::core {

/// 哈希长度（字符数）。
inline constexpr std::size_t kAiUserHashLength = 12;
/// 哈希字符集（小写 base36）。
inline constexpr std::string_view kAiUserHashAlphabet =
    "0123456789abcdefghijklmnopqrstuvwxyz";

/// 由种子生成 AI 用户哈希：SHA-256 后按 base36 截取 kAiUserHashLength 位。
/// @param seed 稳定种子，项目用户传规范绝对项目路径
/// @return 12 位小写 base36 字符串；SHA-256 不可用时为空串
std::string make_ai_user_hash(std::string_view seed);

/// 判断字符串是否为合法 AI 用户哈希。
/// @param value 待校验字符串
/// @return 长度与字符集均合法时为 true
bool is_valid_ai_user_hash(std::string_view value);

} // namespace ai_mirror::core

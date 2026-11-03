#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace ai_mirror::core {

/// AI 用户 home 根目录。
inline constexpr std::string_view kAiUserHomeRoot = "/home";
/// AI 用户 home 目录名前缀。
inline constexpr std::string_view kAiUserHomePrefix = "ai_";
/// 状态文件名。
inline constexpr std::string_view kAiUserStatusFileName = ".am_status";
/// 由哈希生成 home 目录名（ai_ + 哈希）。
/// @param hash 12 位小写 base36 哈希
/// @return home 目录名
std::string ai_user_home_name(std::string_view hash);

/// 由哈希生成 home 目录绝对路径（/home/ai_<hash>）。
/// @param hash 12 位小写 base36 哈希
/// @return home 目录路径
fs::path ai_user_home(std::string_view hash);

/// 由规范绝对项目路径生成 home 目录路径。
/// @param canonical_project_path 已规范化的绝对项目路径
/// @return home 目录路径
fs::path ai_user_home_for_project(const fs::path &canonical_project_path);

/// 判断路径是否为 AI 用户 home（/home/ai_<合法哈希>）。
/// @param path 待判断路径
/// @return 是 AI 用户 home 时为 true
bool is_ai_user_home(const fs::path &path);

/// 从 AI 用户 home 路径提取哈希。
/// @param path 待解析路径
/// @return 合法时返回哈希，否则返回空串
std::string ai_user_hash_from_home(const fs::path &path);

/// home 内状态文件路径。
/// @param home_dir AI 用户 home 目录
/// @return 状态文件路径
fs::path ai_user_status_file(const fs::path &home_dir);

} // namespace ai_mirror::core

// DESIGN BACKGROUND
// AI 用户 home 与项目目录分离后，home 路径、状态文件与 SSH
// 文件位置需要唯一来源， 避免各调用点各自拼接 "/home/ai_..."
// 字符串导致布局漂移。 DESIGN RATIONALE
// 所有布局常量与路径构造函数集中在本文件，调用方只依赖辅助函数。
// EVIDENCE
// tests/unit/test_ai_user_home.cpp 断言布局常量与固定路径；
// commands/user_manager/graft/mount_cleaner 统一改用这些辅助函数。

#include "ai_mirror/core/ai_user_home.hpp"
#include "ai_mirror/core/ai_user_hash.hpp"

namespace ai_mirror::core {

std::string ai_user_home_name(std::string_view hash) {
  return std::string(kAiUserHomePrefix) + std::string(hash);
}

fs::path ai_user_home(std::string_view hash) {
  return fs::path(kAiUserHomeRoot) / ai_user_home_name(hash);
}

fs::path ai_user_home_for_project(const fs::path &canonical_project_path) {
  return ai_user_home(make_ai_user_hash(canonical_project_path.string()));
}

bool is_ai_user_home(const fs::path &path) {
  if (path.parent_path() != fs::path(kAiUserHomeRoot)) {
    return false;
  }
  return is_valid_ai_user_hash(ai_user_hash_from_home(path));
}

std::string ai_user_hash_from_home(const fs::path &path) {
  const std::string name = path.filename().string();
  const std::string prefix(kAiUserHomePrefix);
  if (name.size() != prefix.size() + kAiUserHashLength ||
      name.compare(0, prefix.size(), prefix) != 0) {
    return "";
  }
  const std::string hash = name.substr(prefix.size());
  return is_valid_ai_user_hash(hash) ? hash : "";
}

fs::path ai_user_status_file(const fs::path &home_dir) {
  return home_dir / std::string(kAiUserStatusFileName);
}

} // namespace ai_mirror::core

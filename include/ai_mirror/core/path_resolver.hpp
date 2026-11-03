#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace ai_mirror::core {

class PathResolver {
public:
  static std::optional<fs::path> resolve(const fs::path &p);
  static fs::path to_ai_user_path(const fs::path &main_path,
                                  const std::string &ai_user,
                                  const std::string &main_user,
                                  const fs::path &ai_user_home = {});

  /// 计算 AI 用户访问目标路径时的实际可见路径。
  /// @param target 主用户视角的目标绝对路径
  /// @param project_path 项目根路径，空表示未知
  /// @param home_dir AI 用户 home 目录
  /// @param ai_user AI 用户登录名
  /// @param main_user 主用户登录名
  /// @return 当前布局且目标在项目内返回原路径，否则返回 home 镜像路径
  static fs::path ai_user_access_path(const fs::path &target,
                                      const fs::path &project_path,
                                      const fs::path &home_dir,
                                      const std::string &ai_user,
                                      const std::string &main_user);
  static std::string detect_owner_user(const fs::path &p);
  static std::string detect_ai_user_from_path(const fs::path &p,
                                              const std::string &main_user,
                                              const std::string &prefix);
  static std::optional<fs::path> find_project_root(const fs::path &p);
  static bool is_under_project(const fs::path &path,
                               const fs::path &project_root);
};

} // namespace ai_mirror::core

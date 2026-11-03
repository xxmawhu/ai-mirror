#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ai_mirror::core {

struct SSHKeyEntry {
  std::string name;
  std::string public_key;
};

struct Config {
  struct UserConfig {
    std::string prefix = "i";
    std::vector<fs::path> allowed_bases; // extra allowed base paths beyond
                                         // $HOME (e.g. BeeGFS mount points)
  } user;

  /// 单个挂载项：路径与挂载模式。
  struct MountSpec {
    fs::path path;
    bool read_only{true};
  };

  struct MountConfig {
    std::vector<fs::path> paths;       // 只读挂载列表
    std::vector<fs::path> write_paths; // 可读可写挂载列表
  } mount;

  struct SSHConfig {
    std::string key_type = "ed25519";
    fs::path key_path;
    fs::path ai_default_key;
  } ssh;

  struct AiUserConfig {
    std::vector<std::string>
        groups; // supplementary groups for ai-user (e.g. "docker")
  } ai_user;

  fs::path config_path;
  bool loaded = false;
  std::string load_error;

  /// 返回挂载清单：paths 只读、write_paths 可写，同一路径可写优先。
  /// @return 已去重的挂载项列表（同一路径只出现一次）
  std::vector<MountSpec> mount_specs() const;
};

class ConfigParser {
public:
  static Config load(const fs::path &config_path);
  static Config load_default();
  static bool save(const Config &config, const fs::path &config_path);
  static Config create_default_config(const fs::path &config_path);

private:
  static fs::path expand_path(const std::string &path);
  static fs::path resolve_home(const fs::path &path);
};

} // namespace ai_mirror::core

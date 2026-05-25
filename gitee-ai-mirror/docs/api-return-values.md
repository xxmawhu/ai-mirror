# API 返回值规范

ai-mirror 项目 API 返回值遵循以下规则：

## 1. 查询类：`std::optional<T>`

用于**只读查询**，结果可能不存在，无错误详情需求。

```cpp
std::optional<UserInfo> get_user_info(const std::string& username) const;
std::optional<std::string> derive_username(const fs::path& project_path) const;
```

- 返回 `std::nullopt` 表示查询目标不存在
- 不抛异常，不返回错误字符串

## 2. 写入/操作类：`bool`

用于**修改性操作**（创建、删除、配置写入），成功/失败二元结果。

```cpp
bool remove_ai_user(const std::string& username, bool force = false);
bool bind_mount(const fs::path& source, const fs::path& target, bool read_only = true);
bool generate_key_pair(const fs::path& key_path, const std::string& key_type);
bool save(const Config& config, const fs::path& config_path);
```

- 返回 `true` 表示操作成功
- 返回 `false` 表示操作失败
- 失败详情通过日志 (spdlog) 记录

## 3. 需要详情的结果：结构体带 `error` 字段

用于**需要错误详情**的操作，返回结构体包含状态 + 上下文信息。

```cpp
struct UserInfo {
    std::string username;
    std::string home_dir;
    uid_t uid;
    gid_t gid;
    bool exists;
    std::string error;  // 非空表示失败，包含具体原因
};

struct KeySetupResult {
    bool success = false;
    size_t authorized_count = 0;
    size_t total_count = 0;
};

struct Config {
    // ... fields ...
    bool loaded = false;
    std::string load_error;  // 非空表示加载出错，分号分隔多个错误
};
```

规则：
- `error` / `load_error` 为空字符串表示成功
- 非空时包含具体失败原因（人类可读）
- 部分成功场景提供计数器（如 `authorized_count` vs `total_count`）

## 4. 批量查询：`std::vector<T>`

用于**列表类查询**，返回所有匹配项。

```cpp
std::vector<UserInfo> list_ai_users() const;
std::vector<MountEntry> list_mounts(const std::string& username) const;
```

- 返回空向量表示无匹配项（非错误）

## 5. 清理类：`int`

用于**批量清理**，返回处理的数量。

```cpp
int force_cleanup(const std::vector<fs::path>& dead_mounts);
```

- 返回成功清理的数量
- `-1` 或负数表示部分或完全失败

## 现有 API 对照表

| 类 | 方法 | 返回类型 | 规范类别 |
|---|---|---|---|
| UserManager | `create_ai_user()` | `UserInfo` | 详情 (#3) |
| UserManager | `remove_ai_user()` | `bool` | 写入 (#2) |
| UserManager | `get_user_info()` | `optional<UserInfo>` | 查询 (#1) |
| UserManager | `user_exists()` | `bool` | 查询 (#1)* |
| UserManager | `derive_username()` | `optional<string>` | 查询 (#1) |
| UserManager | `list_ai_users()` | `vector<UserInfo>` | 批量 (#4) |
| ConfigParser | `load()` | `Config` | 详情 (#3) |
| ConfigParser | `save()` | `bool` | 写入 (#2) |
| SSHManager | `generate_key_pair()` | `bool` | 写入 (#2) |
| SSHManager | `authorize_key()` | `bool` | 写入 (#2) |
| SSHManager | `setup_default_keys()` | `KeySetupResult` | 详情 (#3) |
| Graft | `bind_mount()` | `bool` | 写入 (#2) |
| Graft | `unmount()` | `bool` | 写入 (#2) |
| Graft | `list_mounts()` | `vector<MountEntry>` | 批量 (#4) |
| Graft | `force_cleanup()` | `int` | 清理 (#5) |

> *`user_exists()` 返回 `bool` 而非 `optional`，因仅判断存在性，符合查询类的简化形式。

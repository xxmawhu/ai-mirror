# 05 - Bind Mount 模型

## 概述

ai-mirror 使用 Linux bind mount 将主用户的配置文件只读挂载到 AI 用户的 home 目录，使 AI 用户能共享主用户的开发环境配置，同时保持隔离。

## Mount 目标路径计算

```cpp
fs::path PathResolver::to_ai_user_path(
    const fs::path& main_path,      // 主用户路径，如 ~/.bashrc
    const std::string& ai_user,     // AI 用户名，如 i_maxx_a1b2c3
    const std::string& main_user,   // 主用户名，如 maxx
    const fs::path& ai_user_home    // AI 用户 home 目录
);
```

**转换规则**：
- 主用户 home → AI 用户 home
- 保留相对路径结构

**示例**：

| 主用户路径 | AI 用户 home | 目标路径 |
|-----------|-------------|---------|
| `~/.bashrc` | `/mnt/beegfs_data/usr/maxx/ai/project` | `/mnt/beegfs_data/usr/maxx/ai/project/.bashrc` |
| `~/.config/nvim` | `/mnt/beegfs_data/usr/maxx/ai/project` | `/mnt/beegfs_data/usr/maxx/ai/project/.config/nvim` |
| `~/.local/bin/tool` | `/mnt/beegfs_data/usr/maxx/ai/project` | `/mnt/beegfs_data/usr/maxx/ai/project/.local/bin/tool` |

## Mount 流程 (Graft::bind_mount)

```
bind_mount(source, target, read_only=true, uid, gid, home_dir)
  │
  ├── 1. validate_mount_source(source)
  │     ├── SYSTEM_DIRS 黑名单检查
  │     ├── validate_path_exists() (O_PATH|O_NOFOLLOW)
  │     └── fs::canonical() 双重验证（TOCTOU）
  │
  ├── 2. validate_mount_paths(source, target)
  │     ├── 空/相同路径拒绝
  │     ├── 循环 mount 检测
  │     └── 源存在验证
  │
  ├── 3. execute_mount(source, target, read_only, uid, gid, home_dir)
  │     ├── 创建目标（O_CREAT|O_EXCL|O_NOFOLLOW）
  │     ├── chown_path_chain()：修复父目录链 ownership
  │     │   - 从目标到 home_dir 边界向上遍历
  │     │   - 修复所有父目录为 uid:gid
  │     ├── mount --bind source target
  │     ├── 若 read_only=true：
  │     │   - 单步 mount -o ro,bind（内核 >= 5.12）
  │     │   - 失败时回退两步：bind + remount,ro
  │     └── EBUSY → 视为成功（已挂载）
  │
  └── 4. 返回成功/失败
```

## Mount 缓存

```cpp
// graft.hpp
mutable std::vector<MountEntry> mount_cache_;
mutable std::chrono::steady_clock::time_point cache_time_;
static constexpr std::chrono::milliseconds cache_ttl_{500};  // 500ms TTL
```

**缓存机制**：
- `list_mounts()`, `is_mounted()`, `health_check()` 使用缓存
- `is_mounted_live()` 绕过缓存，直接读 `/proc/mounts`
- `invalidate_cache()` 清空缓存

## Mount Entry 结构

```cpp
struct MountEntry {
    fs::path source;     // 挂载源（主用户路径）
    fs::path target;     // 挂载目标（AI 用户 home 内）
    bool read_only;      // 只读标志
    bool active;         // 活跃状态（源存在）
};
```

## Mount 表解析 (parse_mount_table)

```cpp
std::vector<MountEntry> Graft::parse_mount_table() const;
```

**解析流程**：
1. 读取 `/proc/mounts`
2. 过滤 AI 用户 home 下的 mount
3. 通过 passwd lookup 验证 AI 用户名
4. 检查源路径是否存在（lstat）

## Unmount 流程

### unmount(target, lazy=false)

```cpp
bool Graft::unmount(const fs::path& target, bool lazy = false);
```

- 正常 unmount 或 lazy umount (`umount -l`)

### unmount_all(username)

```cpp
bool Graft::unmount_all(const std::string& username);
```

**验证**：
- 用户名必须以 `prefix + main_user + "_"` 开头
- 防止卸载非 ai-mirror 用户的 mount

### cleanup_duplicate_mounts(username)

```cpp
int Graft::cleanup_duplicate_mounts(const std::string& username);
```

**重复 mount 检测**：
- `/proc/mounts` 中同一 target 出现多次 = stacked mount
- 使用 `umount -l` 清理 N-1 层，保留一层

## 写权限授予 (grant_write_access)

```cpp
bool Graft::grant_write_access(const fs::path& path, const std::string& username);
```

**流程**：
1. 创建目录（如不存在）
2. `set_directory_group(path, username)`：chown 到 AI 用户组
3. `chmod g+rwx`：添加组读写执行权限
4. `set_sgid(path)`：设置 S_ISGID 位（新文件继承组）

**效果**：主用户（组成员）可在该目录写入，新文件自动归属 AI 用户组。

## MountCleaner

```cpp
// daemon/mount_cleaner.hpp
class MountCleaner {
    std::vector<fs::path> find_stale_mounts();     // 查找源已删除的 mount
    int force_cleanup(const std::vector<fs::path>&); // 强制清理
    int cleanup_for_user(const std::string& username); // 用户范围清理
};
```

**过期 mount 检测**：
1. 遍历 `/proc/mounts`
2. 检查 mount 是否在 AI 用户 home 下
3. lstat 源路径 → 不存在 = stale

**边界验证**：
- `is_path_under(mount_point, user_home)` 检查精确边界
- 防止 `/home/imaxx_alpha_other` 匹配 `/home/imaxx_alpha`

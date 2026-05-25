# 06 - Ownership 修复机制

## 概述

ai-mirror 在 `create` 和 `update` 命令中执行多轮 ownership 修复，确保 AI 用户 home 目录及其内容正确归属 AI 用户，同时保护 `.am_status` 文件保持 root:root。

## Three-Pass Ownership 修复

### First Pass：Mount 父目录链修复

**位置**：`do_configure()` 第一阶段（mount 时）

**触发条件**：`is_mounted(target)` = true（已挂载目标）

**修复范围**：从 `target.parent_path()` 到 `home_dir` 边界

```cpp
// commands.cpp 269-305
fs::path boundary = home_dir;
fs::path p = target.parent_path();
std::vector<fs::path> to_fix;
while (!p.empty() && p != "/" && p != boundary) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0 && (st.st_uid != state.uid || st.st_gid != state.gid)) {
        to_fix.push_back(p);
    }
    p = p.parent_path();
}
// 从外向内修复（父目录优先）
for (auto it = to_fix.rbegin(); it != to_fix.rend(); ++it) {
    exec_safe({"chown", uid:gid, it->string()});
}
```

**目的**：修复 bind mount 创建时由 root 创建的中间目录（如 `.local/`）

### Second Pass：现有 Mount 父目录修复

**位置**：`do_configure()` 第二阶段（319-348）

**触发条件**：`state.uid != 0 || state.gid != 0`

**修复范围**：所有现有 mount 的父目录链

```cpp
auto all_mounts = ctx.graft->list_mounts(username);
for (const auto& m : all_mounts) {
    fs::path boundary = fs::path(home_dir);
    fs::path p = m.target.parent_path();
    // 遍历父目录链...
}
```

**目的**：处理旧配置遗留的 ownership 问题

### Third Pass：递归修复 home_dir 所有层级

**位置**：`do_configure()` 第三阶段（350-426）

**触发条件**：始终执行

**修复范围**：`home_dir` 及其所有子目录/文件

**排除项**：
1. `.am_status` — 保持 root:root
2. Bind mount 目标 — 不 chown（只读，源归属）

```cpp
// 收集 mount 目标路径
std::set<std::string> mount_targets;
auto all_mounts = ctx.graft->list_mounts(username);
for (const auto& m : all_mounts) {
    mount_targets.insert(m.target.string());
}

// 递归 chown
std::function<int(const fs::path&, int)> recursive_chown = [&](const fs::path& dir_path, int depth) {
    constexpr int max_depth = 256;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        auto ep = entry.path();
        
        // 跳过 .am_status
        if (ep == am_status_path) continue;
        
        // 跳过 bind mount 目标（不 chown，不递归进入）
        if (mount_targets.count(ep.string())) continue;
        
        // symlink：lchown，不递归
        if (S_ISLNK(st.st_mode)) {
            lchown(ep.c_str(), uid, gid);
            continue;
        }
        
        // 目录/文件：fchown
        int fd = open(ep.c_str(), O_RDONLY | O_NOFOLLOW);
        fchown(fd, uid, gid);
        
        // 递归进入子目录
        if (S_ISDIR(st.st_mode)) {
            recursive_chown(ep, depth + 1);
        }
    }
};
```

**最大深度**：256 层

**TOCTOU 防护**：
- O_NOFOLLOW 打开
- fchown 通过 fd 操作（不受路径替换影响）
- symlink 使用 lchown

## chown_path_chain (graft.cpp)

```cpp
static void chown_path_chain(const fs::path& target, uid_t uid, gid_t gid, const fs::path& boundary_dir);
```

**用途**：mount 时修复目标路径的父目录链

**流程**：
1. 从 `target.parent_path()` 向上遍历
2. 到 `boundary_dir`（home_dir）停止
3. 每个父目录 chown 到 uid:gid

## 递归 chown 实现 (chown_recursive_fd)

```cpp
static bool chown_recursive_fd(int dirfd, uid_t uid, gid_t gid, int depth);
```

**用途**：`safe_chown_path()` 使用的 fd-based 递归 chown

**安全特性**：
- `openat(dirfd, name, O_NOFOLLOW)` — 每个组件 O_NOFOLLOW
- `fchownat(..., AT_SYMLINK_NOFOLLOW)` — symlink 安全 chown
- 最大深度 1000 层

**调用位置**：`cmd_cp` 和 `cmd_mv` 复制后递归 chown

## 关键日志消息

| 阶段 | 日志 |
|------|------|
| First pass | `"Fixing ownership for already-mounted target: {}"` |
| Second pass | `"Fixed ownership for existing mount parent: {} -> {}:{}"` |
| Third pass | `"Third pass: fixed {} -> {}:{}"` |
| 跳过 mount | （无日志，静默跳过） |
| 跳过 .am_status | （无日志，静默跳过） |
| 最大深度 | `"Third pass: max depth 256 exceeded at {}"` |

## 修复时机

| 命令 | First | Second | Third |
|------|-------|--------|-------|
| `create` | ✅ | ✅ | ✅ |
| `update` | ✅ | ✅ | ✅ |
| `cp` | — | — | ✅ (safe_chown_path) |
| `mv` | — | — | ✅ (safe_chown_path) |

## 验证方法

```bash
# 查看 home_dir ownership
ls -la /mnt/beegfs_data/usr/maxx/ai/project/

# 查看 .am_status（应为 root:root）
ls -la /mnt/beegfs_data/usr/maxx/ai/project/.am_status

# 递归检查
find /mnt/beegfs_data/usr/maxx/ai/project/ -printf "%u:%g %p\n"
```

## 特殊情况处理

| 情况 | 处理 |
|------|------|
| Bind mount 目标内文件 | 不修复（源归属，只读） |
| Symlink | lchown 修复 symlink 本身，不递归 |
| ELOOP (symlink loop) | lchown fallback |
| 已正确归属 | 跳过（不重复 chown） |
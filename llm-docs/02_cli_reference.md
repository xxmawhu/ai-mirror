# 02 - CLI 命令参考

## 概述

ai-mirror 提供 15 个子命令，通过 `am` shell 函数或直接调用 `ai-mirror-bin` 使用。

**全局选项**：`-v, --verbose`（详细输出）

**前置检查**（除 `init` 外所有命令）：
1. 当前用户不能是 AI 用户（`is_ai_user()` 检查前缀格式）
2. 当前用户必须是 `ai-mirror` 组成员

> **`init` 例外**：init 命令在组检查之前执行，因为它帮助用户设置组成员身份。

---

## 命令列表

### `init` - 初始化用户环境

```bash
am init
```

一键配置当前用户环境，确保 `am` 命令可用。

**执行流程**：
1. 检查 `ai-mirror` 组成员身份（不在组则提示加入命令）
2. 创建用户配置文件 `~/.ai-mirror.toml`（不存在时）
3. 在 `~/.bashrc` 中追加 `source /etc/profile.d/am.sh`（幂等，不重复追加）
4. 加载 bash 补齐

**前置检查豁免**：`init` 在组检查之前执行，帮助用户完成初始设置。

**适用场景**：
- 新用户首次使用 ai-mirror
- tmux 环境下 `am` 命令不可用（`/etc/profile.d/am.sh` 仅登录 shell 加载）
- 环境配置丢失后快速恢复

**退出码**：0 成功 / 1 失败

---

### `create` - 创建 AI 用户

```bash
am create <project_path>
```

为项目创建独立的 AI 用户。需要 root 权限。

**执行流程**：
1. 解析并验证项目路径
2. 创建 Linux 用户（确定性用户名 + UID）
3. 配置环境（SSH、bind mount、组、权限）

**输出**：AI 用户名（stdout）

**退出码**：
- 0：成功（可能有 mount 失败）
- 1：路径无效 / 用户创建失败 / mount 失败

**关键行为**：
- 若用户已存在且 `.am_status` 有效，跳过创建仅重新配置
- 路径遍历修复：为项目路径到主用户 home 之间的所有父目录添加 `g+x`
- 隐私保护：移除主用户 home 下所有目录的 `g+w`

---

### `mkdir` - 授予写权限

```bash
am mkdir <path> <ai_user>
```

授予 AI 用户对指定目录的写权限。

**执行流程**：
1. 验证 AI 用户归属（`validate_ai_user_ownership()`）
2. 验证路径合法性
3. 创建目录（如不存在）
4. 设置组为 AI 用户名 + SGID + g+rwx

**退出码**：0 成功 / 1 失败

---

### `touch` - 创建文件并设置归属

```bash
am touch <path> <ai_user>
```

创建文件并设置 AI 用户为所有者。

**执行流程**：
1. 验证 AI 用户归属
2. 验证路径合法性
3. 创建父目录（如不存在）
4. 以 `O_CREAT | O_EXCL | O_NOFOLLOW` 创建文件（mode 0600）
5. `fchown` 设置归属

**退出码**：0 成功 / 1 失败

---

### `cp` - 复制并设置归属

```bash
am cp <src> <dst>
```

复制文件/目录，自动检测目标路径的 AI 用户并设置归属。

**执行流程**：
1. 验证源和目标路径合法性
2. `cp -rP --no-preserve=mode` 复制
3. `safe_chown_path()` 递归 chown（fd-based 遍历 + O_NOFOLLOW）
4. 清除 setuid/setgid 位

**安全措施**：
- umask 0077 防止中间文件泄露
- 递归 chown 使用 fd-based 遍历防 TOCTOU
- 递归 chmod 清除 S_ISUID | S_ISGID

**退出码**：0 成功 / 1 失败

---

### `mv` - 移动并设置归属

```bash
am mv <src> <dst>
```

原子移动文件/目录，自动检测目标路径的 AI 用户并设置归属。

**执行流程**：
1. 验证源和目标路径合法性
2. 尝试 `fs::rename()`（原子操作）
3. 若跨文件系统，回退到 cp + rm
4. chown 成功确认，失败则回滚 rename（3 次重试）

**回滚机制**：
- rename 后 chown 失败 → 尝试 rename 回原位（3 次重试）
- 回滚失败 → 日志 MANUAL INTERVENTION REQUIRED

**退出码**：0 成功 / 1 失败

---

### `cd` - 切换用户上下文

```bash
am cd <path>
```

智能切换到合适的用户上下文。由 `profile/am.sh` shell 函数处理。

**输出格式**（stdout，key=value）：
```
action=ssh          # AI 用户项目 → SSH 切换
user=imaxx_project  # AI 用户名
path=/path/to/dir   # 目标路径
key=~/.ssh/ai-mirror  # SSH 密钥路径
```

或：
```
action=cd           # 非 AI 项目 → 普通目录切换
path=/path/to/dir
```

**AI 用户检测**：
1. 从目标路径向上遍历查找 `.am_status` 文件
2. 回退：从路径组件中检测 AI 用户名模式（legacy）

**健康检查**：
- 检测到问题（mount 断裂 / SSH 缺失）→ 输出 WARNING 到 stderr
- 输出 debug 行：home、ssh_perms、auth_perms、key_in_auth

**退出码**：0 成功 / 1 失败

---

### `list` - 列出 AI 用户

```bash
am list
```

列出当前主用户管理的所有 AI 用户。

**输出格式**：
```
ai-mirror managed users:
  imaxx_project1 (uid=10000001, home=/path/to/proj1)
    mount: /home/user/.bashrc -> /path/to/proj1/.bashrc
    ...
  imaxx_project2 (uid=10000002, home=/path/to/proj2)
```

---

### `health` - 检查 Mount 健康

```bash
am health
```

检查所有 bind mount 的健康状态。

**检查内容**：
- Mount 源目录是否存在
- Mount 是否活跃

**输出**：通过/失败详情

---

### `force-destroy` - 强制删除 AI 用户

```bash
am force-destroy <project_or_user>
```

强制移除 AI 用户及其所有数据。不可逆。

**执行流程**：
1. 终止用户进程 (`pkill -u`)
2. 卸载所有 bind mount
3. 删除 Linux 用户 (`userdel`)
4. 删除 home 目录
5. 撤销写权限授予

---

### `rm` - 安全删除项目用户

```bash
am rm <project_path>
```

移除项目 AI 用户，保留输出文件。

**执行流程**：
1. 验证路径和用户归属
2. 终止用户进程
3. 卸载 bind mount
4. 保留输出文件，删除用户

---

### `config` - 显示配置

```bash
am config
```

显示当前加载的配置信息。

---

### `status` - 显示项目状态

```bash
am status
```

显示所有项目的状态摘要。

---

### `update` - 重新应用修复

```bash
am update <project_path>
```

重新应用 SSH 和 mount 修复。等同于 `create` 的配置阶段（`do_configure()`）。

**适用场景**：
- mount 意外断开
- SSH 授权丢失
- ownership 错误
- 配置变更后重新应用

---

### `watch` - 实时监控

```bash
am watch
```

htop 风格的 TUI 实时监控（基于 FTXUI）。

**显示内容**：
- 所有 AI 用户列表
- Mount 状态
- 健康状态

## 返回码约定

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 一般错误（路径无效、权限不足、操作失败） |
| 其他 | CLI 解析错误（由 CLI11 处理） |

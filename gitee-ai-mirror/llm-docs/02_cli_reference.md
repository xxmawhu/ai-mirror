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
4. 检查 bash 补齐是否安装

**前置检查豁免**：`init` 在组检查之前执行，帮助用户完成初始设置。

**适用场景**：
- 新用户首次使用 ai-mirror
- tmux 环境下 `am` 命令不可用（`/etc/profile.d/am.sh` 仅登录 shell 加载）
- 环境配置丢失后快速恢复

**输出示例**：
```
=== ai-mirror 初始化检查 ===

1. ai-mirror 组成员身份: ✓ 已在组中

2. 用户配置文件 ~/.ai-mirror.toml: ○ 不存在（将使用默认配置）
   → 已创建默认配置文件

3. ~/.bashrc 中 am.sh 加载: ○ 未配置
   → 已追加配置到 ~/.bashrc

4. Bash 补齐: ✓ 已安装

=== 总结 ===
环境已完整配置。
新终端/tmux 窗口可直接使用 am 命令。
```

**退出码**：0 成功 / 1 需手动修复（如未加入组）

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
4. 双向组添加：主用户↔AI 用户互加组
5. 附加组：从 `[ai-user].groups` 配置添加（如 docker）

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

授予 AI 用户对指定目录的写权限。需要 root 权限。

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

创建文件并设置 AI 用户为所有者。需要 root 权限。

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

复制文件/目录，双向检测 AI 用户并设置归属。

**执行流程**：
1. 验证源和目标路径合法性
2. 检测源路径和目标路径的 AI 用户归属
3. `cp -rP --no-preserve=mode` 复制
4. 根据 src/dst 的 AI 用户检测结果决定 chown 目标：
   - dst 在 AI 用户目录 → chown 给 AI 用户
   - src 在 AI 用户目录，dst 在主用户目录 → chown 回主用户
   - 两者都在主用户目录 → 不做 chown

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

原子移动文件/目录，双向检测 AI 用户并设置归属。

**执行流程**：
1. 验证源和目标路径合法性
2. 检测源路径和目标路径的 AI 用户归属
3. 尝试 `fs::rename()`（原子操作）
4. 若跨文件系统，回退到 cp + rm
5. 根据 src/dst 双向 AI 用户检测决定 chown 目标
6. chown 失败则回滚 rename（3 次重试）

**chown 决策模型**（同 `cp`）：
- main→ai：chown 给 AI 用户
- ai→main：chown 回主用户
- 同主用户的 ai↔ai：允许
- 跨主用户：拒绝

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

列出当前主用户管理的所有 AI 用户及其 bind mount 信息。

**输出格式**：
```
ai-mirror managed users:
  i_maxx_a1b2c3 (uid=10000001, home=/path/to/proj1)
  i_maxx_d4e5f6 (uid=10000002, home=/path/to/proj2)
```

**退出码**：0 成功

---

### `health` - 检查 Mount 健康

```bash
am health
```

检查所有 bind mount 的健康状态。

**检查内容**：
- Mount 源目录是否存在
- Mount 是否活跃

**输出**：`[OK]` 或 `[FAIL]` + 详情

**退出码**：0 全部健康 / 1 有异常

---

### `force-destroy` - 强制删除 AI 用户

```bash
am force-destroy <project_or_user>
```

强制移除 AI 用户及其所有数据。不可逆。需要 root 权限。

**与 `rm` 的区别**：`force` 标志传递给 `userdel`，强制删除即使用户有进程运行。

**执行流程**：
1. 解析用户名（接受项目路径或用户名）
2. 验证用户归属（属于当前主用户）
3. 卸载所有 bind mount
4. 强制删除 Linux 用户 (`userdel -f`)
5. 删除 home 目录

**退出码**：0 成功 / 1 失败

---

### `rm` - 安全删除项目用户

```bash
am rm <project_path>
```

移除项目 AI 用户，保留输出文件。需要 root 权限。

**执行流程**：
1. 解析项目路径，推导 AI 用户名
2. 验证用户归属（属于当前主用户）
3. 卸载所有 bind mount
4. 终止用户进程 (`pkill -u`)
5. 删除 Linux 用户（`userdel`，保留 home 目录内容）
6. 清理 AI 用户 home 目录
7. 撤销项目路径上的写权限授予

**输出**：`Removed: <ai_user>`

**退出码**：0 成功 / 1 失败

---

### `config` - 显示配置

```bash
am config
```

显示当前加载的配置信息。

**输出内容**：
- 配置文件路径
- `[user]` prefix、allowed_bases
- `[ssh]` key_type、key_path、ai_default_key
- `[mount]` paths
- `[ai-user]` groups（附加组列表）
- 加载状态（yes/no）

**示例输出**：
```
Config file: ~/.ai-mirror.toml
User prefix: i (default)
SSH key type: ed25519
SSH key path: ~/.ssh/id_ed25519
SSH default key: ~/.ssh/id_ed25519.pub
Mount paths:
  - ~/.bashrc
  - ~/.config
  - ~/.local/bin
AI-user groups:
  - docker
Loaded: yes
```

---

### `status` - 显示项目状态

```bash
am status
```

显示当前主用户管理的所有项目状态摘要。

**输出内容**（每个项目）：
- 用户名、Home 目录、UID
- Mount 列表（源→目标，ro/rw，active/broken）
- SSH 密钥状态（ok/missing）
- authorized_keys 状态（ok/missing）
- 综合健康状态（healthy/unhealthy）

---

### `update` - 重新应用修复

```bash
am update <project_path>
```

重新应用 SSH 和 mount 修复。需要 root 权限。等同于 `create` 的配置阶段（`do_configure()`）。

**适用场景**：
- mount 意外断开
- SSH 授权丢失
- ownership 错误
- 配置变更后重新应用

**执行流程**（同 `create` 的 do_configure）：
1. 解析项目路径，验证 AI 用户存在
2. 重新配置 SSH 密钥
3. 重新应用 bind mount
4. First pass: 修复 `.am_status` 所有权（root:root）
5. Second pass: 修复父目录权限
6. Third pass: 递归修复 home 目录所有权
7. 授予项目路径写权限

---

### `watch` - 实时监控

```bash
am watch
```

htop 风格的 TUI 实时监控（基于 FTXUI）。按 Ctrl+C 退出。

**显示内容**：
- 所有 AI 用户列表
- Mount 状态（ro/rw、active/broken）
- SSH 和 authorized_keys 健康状态
- 实时刷新（后台线程）

**输出**：交互式 TUI 界面，退出时打印 `Watch stopped.`

## 返回码约定

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 一般错误（路径无效、权限不足、操作失败） |
| 其他 | CLI 解析错误（由 CLI11 处理） |

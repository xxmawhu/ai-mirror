# ai-mirror

Linux 用户权限隔离管理工具。为每个项目创建独立的 Linux 用户，通过 Bind Mount 只读共享配置文件，SSH 免密登录切换身份，让 AI agent 以最小权限运行。

## 核心概念

| 角色 | 示例 | 说明 |
|------|------|------|
| 主用户 | `maxx` | 拥有 sudo 权限，管理所有 ai-user |
| AI用户 | `imaxx_alpha` | 每个项目一个，仅拥有该项目目录的写权限 |

权限模型：
- 主用户的配置文件（`.bashrc`, `.config` 等）通过 bind mount 只读挂载到 ai-user home
- ai-user 仅对分配的项目目录有读写权限
- 通过 SSH 免密登录切换身份，不使用 ACL

## 安装

### 从源码构建

```bash
# 依赖: g++ 13+, cmake 3.28+
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

构建启用了安全加固标志：PIE、Full RELRO、Stack Protector、FORTIFY_SOURCE。

### 系统级安装

```bash
sudo ./install.sh                # 完整安装 (构建 + 部署)
sudo ./install.sh --build        # 仅构建
sudo ./install.sh --clean        # 卸载
sudo ./install.sh --skip-pull    # 安装（跳过 git pull）
```

安装后提供两个入口：
- `am` — wrapper 脚本，自动处理 sudo 提权
- `ai-mirror-bin` — 实际二进制，通过 sudoers 规则提权

### 运行环境

- Linux（systemd 或非 systemd 环境均可）
- Docker 集成测试: Ubuntu 22.04 / 24.04

## 使用

### 前置条件

- 当前用户必须属于 `ai-mirror` 组
- AI 用户不能执行任何命令（会被自动拦截）
- 需要提权的命令通过 `am` wrapper 自动处理 sudo

### 全局选项

| 选项 | 说明 |
|------|------|
| `-v, --verbose` | 显示详细输出 |

---

### `create` — 创建项目用户

```bash
am create <project_path>
```

为指定项目创建独立的 ai-user，完成全部初始化。

**适用情形**: 首次为项目启用隔离环境时使用。

**执行流程**:
1. 创建 Linux 用户（用户名 = `prefix + main_user + "_" + 项目名哈希`）
2. 设置 SSH 免密登录（生成密钥对 + authorized_keys）
3. 只读 bind mount 主用户的配置文件（`.bashrc`, `.config` 等）
4. 授予项目目录读写权限
5. 输出创建的用户名

**条件**:
- 需要 root 权限（通过 sudoers 自动提权）
- `project_path` 必须在主用户 home 目录下
- 用户名碰撞时拒绝创建并报错
- 路径含 `..` 或无法 canonicalize 时拒绝

---

### `mkdir` — 授权目录

```bash
am mkdir <path> <ai_user>
```

创建目录并授权 ai-user 读写。

**适用情形**: 需要给 ai-user 额外的写入目录（如输出目录、缓存目录）。

**条件**:
- 需要 root 权限
- `path` 必须在主用户 home 目录下
- `ai_user` 必须属于当前主用户（ownership 验证）
- 目录已存在时直接授权，不报错

---

### `touch` — 创建文件并授权

```bash
am touch <path> <ai_user>
```

创建空文件并设置 ai-user 所有权。

**适用情形**: 需要授予 ai-user 对单个文件的写权限，或预创建配置/状态文件。

**条件**:
- 需要 root 权限
- `path` 必须在主用户 home 目录下
- `ai_user` 必须属于当前主用户
- 文件已存在时直接修改所有权
- 父目录不存在时自动递归创建
- 使用 `O_NOFOLLOW + fchown` 防 TOCTOU 符号链接攻击

---

### `cp` — 复制文件

```bash
am cp <src> <dst> -u <ai_user>
```

复制文件或目录，并设置 ai-user 所有权。

**适用情形**: 需要将配置文件、数据文件复制到 ai-user 可写的目录。

**条件**:
- 需要 root 权限
- `src` 和 `dst` 都必须在主用户 home 目录下
- `src` 必须存在
- `ai_user` 必须属于当前主用户
- 使用 `cp -r --no-preserve=mode`：不保留源文件权限模式
- 自动清除 SUID/SGID 位，防止提权
- 使用 `O_NOFOLLOW + fchown` 防 TOCTOU 符号链接攻击

---

### `mv` — 移动文件

```bash
am mv <src> <dst> -u <ai_user>
```

移动文件或目录，并设置 ai-user 所有权。

**适用情形**: 需要将文件重新组织到 ai-user 的工作目录。

**执行逻辑**:
- 同文件系统：原子 rename + chown
- 跨文件系统：copy + delete（回退方案）

**条件**:
- 需要 root 权限
- `src` 和 `dst` 都必须在主用户 home 目录下
- `src` 必须存在
- `ai_user` 必须属于当前主用户
- 跨设备复制时不保留 SUID/SGID 位
- 使用 `O_NOFOLLOW + fchown` 防 TOCTOU 符号链接攻击

---

### `cd` — 切换身份

```bash
am cd <path>
```

根据目标路径所属用户，自动切换到对应的身份上下文。

**适用情形**: 从主用户 shell 快速切换到 ai-user 工作环境。

**执行逻辑**:
- 目标属于主用户 → 输出 `cd <path>`
- 目标属于当前主用户的 ai-user → 输出 `exec ssh <ai-user>@localhost -t "cd <path> && exec bash -l"`
- 其他情况 → 输出 `cd <path>`

**条件**:
- 不需要 root 权限
- `path` 必须存在
- 路径含 shell 元字符时拒绝（防命令注入）
- SSH 目标和路径均经过 shell_escape 转义

> **注意**: `am cd` 输出的是 shell 命令，需要通过 `eval $(am cd <path>)` 执行，或由 wrapper 脚本自动 eval。

---

### `rm` — 删除项目

```bash
am rm <project_path>
```

安全删除项目的 ai-user，保留输出文件。

**适用情形**: 项目结束，需要清理隔离环境但保留项目目录中的文件。

**执行流程**:
1. 验证 ai-user 归属当前主用户
2. 卸载所有 bind mount
3. 删除 ai-user（保留 home 中的数据文件）
4. 清理 ai-user home 目录
5. 撤销项目目录的写权限

**条件**:
- 需要 root 权限
- `project_path` 必须能推导出有效的 ai-user
- ai-user 必须存在
- ai-user 必须属于当前主用户（ownership 验证）

---

### `force-destroy` — 强制清理

```bash
am force-destroy <username_or_project_path>
```

强制卸载并删除 ai-user，不保留任何数据。

**适用情形**: 当 `rm` 无法正常工作（用户损坏、mount 异常等）时的紧急清理。

**条件**:
- 需要 root 权限
- 参数可以是 ai-user 用户名或项目路径
- ai-user 必须属于当前主用户（ownership 验证）
- **不保留任何数据**，比 `rm` 更彻底

---

### `list` — 列出用户

```bash
am list
```

列出所有由 ai-mirror 管理的 ai-user 及其 bind mount 状态。

**适用情形**: 查看当前系统中有哪些 ai-user，以及各自的挂载情况。

**条件**:
- 不需要 root 权限

---

### `status` — 项目状态

```bash
am status
```

显示所有 ai-user 的详细信息：home、UID、mount 状态、SSH 密钥、authorized_keys 健康状态。

**适用情形**: 排查 mount 断开、SSH 密钥缺失等问题。

**条件**:
- 不需要 root 权限
- 显示 mount 的 active/broken 状态和 SSH 密钥是否存在

---

### `health` — 健康检查

```bash
am health
```

检查所有 bind mount 是否正常。退出码 0 = 全部健康，1 = 存在异常。

**适用情形**: 监控和告警，可集成到 cron 或 systemd timer。

**条件**:
- 不需要 root 权限

---

### `config` — 查看配置

```bash
am config
```

显示当前加载的配置文件路径及所有配置项。

**适用情形**: 确认配置是否正确加载，调试配置问题。

**条件**:
- 不需要 root 权限

## 配置文件

用户配置 `~/.ai-mirror.toml`（首次运行时自动创建）。`prefix` 固定为 `"i"`（系统级，不可配置）：

```toml
# 用户级配置 (~/.ai-mirror.toml)
[mount]
paths = [
    "~/.bashrc",       # 只读挂载到每个 ai-user
    "~/.config",
]

[ssh]
key_type = "ed25519"
key_path = "~/.ssh/ai-mirror"              # 用于 SSH 登录 ai-user 的密钥对
ai_default_key = "~/.ssh/id_ed25519.pub"   # 自动读取公钥并授权给 ai-user（用于读取远程 GitLab 仓库）
```

## Sudoers 配置

安装时自动创建 `/etc/ai-mirror/sudoers.d/ai-mirror`，仅允许 `ai-mirror` 组用户通过 `sudo` 执行指定命令：

```
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin create
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin mkdir
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin touch
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin cp
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin mv
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin cd
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin rm
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin force-destroy
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin health
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin list
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin config
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin status
```

安全要点：
- 无通配符，仅列出命令名，参数验证由 C++ 二进制层强制执行
- 所有文件操作路径限制在主用户 home 目录内（home-only allowlist）
- ai-user 归属验证确保不同主用户之间隔离
- 需要将用户加入 `ai-mirror` 组：`sudo usermod -aG ai-mirror $USER`

## 架构

```
src/
├── main.cpp              # CLI 入口
├── cli/
│   ├── parser.cpp        # CLI11 命令解析
│   └── commands.cpp      # 命令分发
├── core/
│   ├── user_manager.cpp  # Linux 用户管理 (useradd/userdel)
│   ├── graft.cpp         # Bind Mount 管理
│   ├── ssh_manager.cpp   # SSH 密钥管理
│   ├── path_resolver.cpp # 路径安全检查
│   └── config.cpp        # TOML 配置解析
├── daemon/
│   ├── health_check.cpp  # 心跳检测
│   ├── auth_monitor.cpp  # auth.log 监控
│   └── mount_cleaner.cpp # 挂载清理
├── security/
│   ├── path_validator.cpp # 路径包含性检查
│   └── audit.cpp          # 安全审计
└── utils/
    ├── shell.cpp          # Shell 工具函数
    └── logger.cpp         # 日志
```

### 依赖 (FetchContent 自动获取)

| 库 | 版本 | 用途 |
|----|------|------|
| CLI11 | v2.4.2 | CLI 解析 |
| toml11 | v4.2.0 | TOML 配置 |
| spdlog | v1.15.0 | 日志 |
| nlohmann_json | v3.11.3 | JSON 处理 |

## 安全设计

### 路径验证

- **Home-only 允许列表**: 所有文件操作命令（`create`, `mkdir`, `touch`, `cp`, `mv`）的路径参数必须在主用户 home 目录下，其他路径一律拒绝
- **路径包含性检查**: mount 前验证 Target 不是 Source 的子目录，防止循环挂载
- **符号链接防护**: `is_path_allowed()` 在 canonical 失败时返回失败（不回退原始路径），使用 `fs::path` 迭代器检测 `..` 组件
- **路径存在性验证**: `validate_path_exists()` 使用 `O_PATH | O_NOFOLLOW` + `fstat` 确认路径存在且为目录/文件
- **Mount 前二次验证**: `bind_mount()` 在验证和执行之间再次检查 canonical 路径一致性，防止 TOCTOU 攻击

### 所有权验证

- **ai-user 归属检查**: 所有涉及 ai-user 的命令（`mkdir`, `touch`, `cp`, `mv`, `rm`, `force-destroy`）均调用 `validate_ai_user_ownership()`，验证 ai-user 名称以 `prefix + main_user` 开头
- **防止跨用户操作**: 同一 `ai-mirror` 组内的不同主用户无法操作彼此的 ai-user

### TOCTOU 防护

- **fd-based chown**: `cmd_touch` 使用 `open(O_NOFOLLOW) + fchown()` 获取 fd 后再修改所有权，消除符号链接替换攻击窗口
- **递归安全 chown**: `cmd_cp` 和 `cmd_mv` 使用 `safe_chown_path()`，对每个文件用 `open(O_NOFOLLOW) + fchown()`，对目录递归遍历处理，目录自身的 ELOOP 回退为普通 chown
- **SUID/SGID 清除**: `safe_chown_path()` 在 chown 后自动调用 `chmod ug-s` 清除所有 SUID/SGID 位

### SUID/SGID 防护

- **cp 不保留模式**: `cmd_cp` 和 `cmd_mv`（跨文件系统时）使用 `cp -r --no-preserve=mode` 而非 `cp -a`，不保留源文件的 SUID/SGID 位和权限模式
- **二次清除**: chown 后额外执行 `chmod -R ug-s`，防御性清除所有 SUID/SGID 位

### 权限隔离

- **只读 Bind Mount**: 配置文件以只读方式挂载，ai-user 无法修改
- **无 ACL**: 仅使用传统 Linux 组权限（groupadd, chgrp, chmod g+rwX, SGID）
- **权限撤销完整**: `revoke_write_access()` 清除 SGID 位、从组中移除用户、删除用户组

### 身份与认证

- **SSH 隔离**: 通过 SSH 密钥切换身份，非 su/sudo
- **身份验证**: 优先读取 `/proc/self/loginuid`，防止 `SUDO_USER` 环境变量欺骗
- **用户名碰撞拒绝**: `generate_username()` 截断后检测系统用户碰撞，碰撞时直接返回错误并拒绝创建，不追加后缀

### 命令注入防护

- **SSH 命令注入防护**: `cmd_cd` 对整个远程命令进行 `shell_escape()`，使用 `&&` 替代 `;`，路径含 shell 元字符时拒绝
- **Shell 注入防护**: `fork()+execv()` 替代 `popen()`，SSH 输出使用 `shell_escape()`
- **TOML 注入防护**: 配置写入时 `toml_escape()` 转义特殊字符
- **绝对路径执行**: `resolve_command()` 将命令名解析为绝对路径，防止 PATH 劫持

### 编译器加固

PIE、Full RELRO、Stack Protector、FORTIFY_SOURCE、NX (`-Wl,-z,noexecstack`)

### Sudoers 安全模型

sudoers 规则仅列出命令名（无通配符），参数验证由 C++ 二进制层强制执行：

- 所有路径参数经过 `validate_path_allowed()` / `validate_mount_source()` 验证
- 系统目录 (`/etc`, `/root`, `/var`, `/proc`, `/sys`, `/dev`, `/boot`, `/lib`, `/usr`, `/sbin`, `/bin`, `/run`) 被拒绝
- `force_cleanup()` 仅允许卸载 `/home/` 下的挂载点
- `cmd_rm` 的 find 扫描范围限制在用户 home 和项目目录内

### 安全审计

`audit_mounts_for_user()`, `audit_user_permissions()`, `full_audit()` 提供运行时安全审计能力。

## 测试

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Docker 集成测试（26 项全部通过）：

```bash
docker build -t ai-mirror-ubuntu24 -f docker/Dockerfile.ubuntu24 .
docker build -t ai-mirror-ubuntu22 -f docker/Dockerfile.ubuntu22 .
```

## 项目状态

- [x] Phase 1: 核心基础 — 用户管理、路径安全、Bind Mount、SSH、CLI
- [x] Phase 2: 高级功能 — 配置文件、cd 自动切换、mkdir/touch 权限、cp/mv、心跳检测、强制清理
- [x] Phase 3: 运维与测试 — Docker 集成测试、install.sh 部署、安全审计
- [x] 安全加固 (SECURITY-001 ~ SECURITY-018, SEC-019 ~ SEC-022): 22 项安全问题全部修复

## License

Private

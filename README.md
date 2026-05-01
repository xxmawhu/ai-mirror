# ai-mirror

**AI 时代的 Linux 用户隔离方案 — 让每个 AI Agent 在自己的沙盒里安全工作。**

当 AI Agent 拥有了写文件、执行命令的能力，安全问题就不再是理论探讨，而是每天都要面对的现实。一个失控的 `rm -rf`，一个越权修改的 `.bashrc`，一次对其他项目目录的意外写入——这些不是假设，而是在"给 AI 完整 shell 权限"模式下每天都在发生的事。

**ai-mirror 给出了一个务实的答案：不需要容器，不需要虚拟机，用 Linux 原生的用户权限体系就能做到真正的隔离。**

每个 AI 项目获得一个独立的 Linux 用户。这个用户只能写自己项目的目录，配置文件从主用户只读挂载，身份切换通过 SSH 密钥完成。没有 ACL 的复杂性，没有 Docker 的开销，没有 rootless 的妥协。就是 Linux 权限模型本该有的样子。

## 为什么需要它？

| 场景 | 没有 ai-mirror | 有 ai-mirror |
|------|---------------|-------------|
| AI Agent 误操作 | 可能影响整个 home 目录 | 只影响单个项目目录 |
| 多项目并行 | 同一用户下文件互相可见 | 每个项目独立用户，互相不可见 |
| 配置被篡改 | Agent 可以改 `.bashrc`, `.ssh` 等 | 只读 bind mount，无法修改 |
| 权限审计 | 需要额外工具追踪 | Linux 标准用户权限，`ls -l` 即可 |
| 资源开销 | 容器方案占用 GB 级别 | 零额外资源，只是多一个 Linux 用户 |

## 核心概念

| 角色 | 示例 | 说明 |
|------|------|------|
| 主用户 | `maxx` | 拥有 sudo 权限，管理所有 ai-user |
| AI用户 | `imaxx_alpha` | 每个项目一个，仅拥有该项目目录的写权限 |

权限模型：
- 主用户的配置文件（`.bashrc`, `.config` 等）通过 bind mount 只读挂载到 ai-user home
- ai-user 仅对分配的项目目录有读写权限
- 通过 SSH 免密登录切换身份，不使用 ACL

## 快速开始

### 构建

```bash
# 依赖: g++ 13+, cmake 3.28+
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

构建启用了安全加固标志：PIE、Full RELRO、Stack Protector、FORTIFY_SOURCE。

### 安装（推荐）

`install.sh` 是一键构建+部署脚本，自动完成以下全部工作：

```bash
chmod +x install.sh

# 完整安装（构建 + 部署到系统）
sudo ./install.sh

# 仅构建，不安装
sudo ./install.sh --build

# 卸载
sudo ./install.sh --clean
```

**`install.sh` 执行流程**：

| 阶段 | 操作 |
|------|------|
| 依赖检查 | 自动检测并安装缺失的系统包（cmake, g++, make, git, openssh-server, sudo） |
| 构建 | CMake Release 编译，增量构建支持 |
| 验证 | 检查二进制文件完整性 + `--help` 冒烟测试 |
| 安装 | 部署 `ai-mirror-bin` 到 `/usr/local/bin/` |
| Profile | 安装 `am()` bash 函数到 `/etc/profile.d/am.sh`（登录自动加载） |
| Sudoers | 创建 `/etc/ai-mirror/sudoers.d/ai-mirror`，无通配符白名单规则 |
| 用户组 | 创建 `ai-mirror` 系统组 |

**可自定义路径**（环境变量）：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `AI_MIRROR_PREFIX` | `/usr/local` | 二进制安装前缀 |
| `AI_MIRROR_CONFIG_DIR` | `/etc/ai-mirror` | 配置文件目录 |
| `AI_MIRROR_DATA_DIR` | `/var/lib/ai-mirror` | 数据目录 |

安装日志保存在 `./log/install.log`。

### 运行环境

- Linux（任何发行版）
- Docker 集成测试: Ubuntu 22.04 / 24.04

## 使用

### 前置条件

- 当前用户必须属于 `ai-mirror` 组：`sudo usermod -aG ai-mirror $USER`
- AI 用户不能执行任何命令（会被自动拦截）
- 需要提权的命令通过 `am` shell 函数自动处理 sudo
- **bash 用户**：安装后新登录自动加载 `am()` shell 函数；或手动 `source /etc/profile.d/am.sh`

### Bash 自动补齐

`am` 支持 bash 命令补齐，类似 git 的 tab 补全体验。

**安装方式**：

```bash
# 方式1: 手动加载（临时测试）
source completions/am-completion.bash

# 方式2: 系统级安装（推荐）
sudo cp completions/am-completion.bash /etc/bash_completion.d/am

# 方式3: 用户级安装（无需 root）
mkdir -p ~/.local/share/bash-completion/completions
cp completions/am-completion.bash ~/.local/share/bash-completion/completions/am
```

安装后新终端自动生效，或执行 `source ~/.bashrc` 立即启用。

**补齐功能**：
- 子命令补齐：`am <tab>` → create, cd, rm, list, status, health, update...
- 参数补齐：`am create <tab>` → 项目目录补齐
- 用户名补齐：`am rm <tab>` → 已创建的 ai-user 名称补齐
- 文件路径补齐：基于当前目录的文件/目录补齐

### sudoers 安全说明

虽然 sudoers 规则允许 `ai-mirror` 组无密码执行 `am`，但程序内部实现了多层验证：
- 路径验证 (`is_path_allowed`) 拒绝非 `/home/` 路径
- 用户名验证 (`validate_username`) 拒绝 shell 元字符注入
- 配置文件所有权检查防止预置恶意配置

建议定期审计 `ai-mirror` 组成员，确保仅授权用户可执行特权命令。

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

**执行流程**:
1. 创建 Linux 用户（用户名 = `prefix + main_user + "_" + 项目名哈希`）
2. 设置 SSH 免密登录（生成密钥对 + authorized_keys）
3. 只读 bind mount 主用户的配置文件（`.bashrc`, `.config` 等）
4. 授予项目目录读写权限
5. 输出创建的用户名

---

### `mkdir` — 授权目录

```bash
am mkdir <path> <ai_user>
```

创建目录并授权 ai-user 读写。

---

### `touch` — 创建文件并授权

```bash
am touch <path> <ai_user>
```

创建空文件并设置 ai-user 所有权。使用 `O_NOFOLLOW + fchown` 防符号链接攻击。

---

### `cp` — 复制文件

```bash
am cp <src> <dst>
```

复制文件或目录，自动检测目标路径是否属于 ai-user 目录。若属于 ai-user 目录，自动设置所有权；否则执行普通复制。自动清除 SUID/SGID 位。

> **非 ai-user 目标警告**: 若目标不在任何 ai-user 目录下，将输出警告并建议使用普通 `cp` 命令。这是因为非 ai-user 目录无需所有权设置，`am cp` 的主要价值在于自动权限管理。

---

### `mv` — 移动文件

```bash
am mv <src> <dst>
```

移动文件或目录，自动检测目标路径是否属于 ai-user 目录。同文件系统原子 rename，跨文件系统 copy+delete。

---

### `cd` — 切换身份

```bash
am cd <path>
```

根据目标路径所属用户，自动切换到对应的身份上下文：

- **普通目录**：在当前 shell 中执行 `cd`，改变工作目录
- **ai-user 项目目录**：通过 SSH 登录到 AI 用户，并 `cd` 到目标路径

> **跨共享盘支持**: ai-user 检测基于路径结构而非 UID，在 NFS/BeeGFS 等共享存储上也能正确识别。

---

### `rm` — 删除项目

```bash
am rm <project_path>
```

安全删除项目的 ai-user，保留输出文件。

---

### `force-destroy` — 强制清理

```bash
am force-destroy <username_or_project_path>
```

强制卸载并删除 ai-user，不保留任何数据。用于异常情况下的紧急清理。

> **输入识别**: 若输入符合有效用户名格式，直接使用该用户名；否则尝试从路径推导用户名。这避免了用户名碰撞问题——显式输入用户名时不会发生意外推导。

---

### `list` — 列出用户

```bash
am list
```

列出当前用户所拥有的 ai-user 及其 bind mount 状态（仅显示属于调用者的 ai-user，其他用户的不可见）。

---

### `status` — 项目状态

```bash
am status
```

显示当前用户的 ai-user 详细信息：home、UID、mount 状态、SSH 密钥、authorized_keys 健康状态（仅显示属于调用者的 ai-user）。

---

### `health` — 健康检查

```bash
am health
```

检查所有 bind mount 是否正常。退出码 0 = 全部健康，1 = 存在异常。可集成到 cron 定时任务。

---

### `config` — 查看配置

```bash
am config
```

显示当前加载的配置文件路径及所有配置项。

## 配置文件

默认配置编译在二进制中（mount `~/.bashrc` + `~/.config`，SSH ed25519），无需配置文件即可运行。

用户级配置 `~/.ai-mirror.toml`（首次运行自动创建，用于覆盖默认值）：

```toml
# [user] 用户创建与路径限制配置
# allowed_bases: 额外允许创建 ai-user 的路径白名单
#   - 默认只允许在主用户 $HOME 下创建 ai-user
#   - HPC 共享存储场景（如 BeeGFS）需要配置此项
#   - 支持 {user} 占位符，运行时替换为当前用户名
#   - 白名单路径仍需通过 ownership/权限验证 + SYSTEM_DIRS 黑名单检查
#
# 示例：允许在 BeeGFS 共享存储创建 ai-user
# [user]
# prefix = "i"
# allowed_bases = ["/mnt/beegfs_data/usr/{user}", "/scratch/{user}"]
#
# 注意：prefix 在编译时已设置默认值，通常无需配置

# [mount] 只读挂载到每个 ai-user home 目录的文件/目录
# ai-user 通过 bind mount 以只读方式看到这些内容，无法修改。
# 用途：让 ai-user 继承主用户的 shell 环境、编辑器配置、语言设置等，
#       避免每个 ai-user 都需要单独配置一遍。
# 路径支持 ~ 展开为当前主用户的 home 目录。
[mount]
paths = [
    "~/.bashrc",       # Shell 初始化脚本（别名、环境变量、PATH 等）
    "~/.config",       # 应用配置目录（git、nvim、bash-completion 等）
    "~/.local/bin",    # 用户自定义脚本/工具目录
]

# [ssh] SSH 密钥与身份切换配置
# ai-mirror 通过 SSH 密钥实现主用户到 ai-user 的免密身份切换，
# 而非 su/sudo，从而完全避免 ai-user 获取任何提权路径。
#
# key_path: 主用户登录 ai-user 时使用的密钥对（私钥 + 公钥）
#           此密钥对由 ai-mirror 自动生成并管理。
#           主用户执行 `am cd <project_path>` 时，
#           底层使用此密钥 SSH 到 ai-user@localhost。
#           不配置时自动检测：id_ed25519 > id_rsa > id_ecdsa > ai-mirror
#
# ai_default_key: ai-user 的身份密钥（identity key）
#                 典型场景：让 ai-user 能以自己的身份读取远程 Git 仓库
#                 （GitLab、GitHub 等），而不需要主用户的完整 SSH 权限。
#                 ai-mirror 会复制此密钥到 ai-user ~/.ssh/，并自动检测格式
#                 （ed25519/rsa/ecdsa），重命名为 SSH 默认文件名（id_ed25519/id_rsa/id_ecdsa）。
#                 支持配置公钥路径（.pub）或私钥路径，两者都会被复制。
#                 注意：此密钥作为 ai-user 的身份密钥，不会添加到 authorized_keys。
#
# key_type: 密钥算法，推荐 ed25519（更短、更安全、更快）
[ssh]
key_type = "ed25519"
# key_path = "~/.ssh/ai-mirror"  # 可选，不配置则自动检测现有 SSH key
ai_default_key = "~/.ssh/id_ed25519.pub"
```

## 架构

```
src/
├── main.cpp              # CLI 入口
├── cli/
│   ├── parser.cpp        # CLI11 命令解析
│   └── commands.cpp      # 命令分发
├── core/
│   ├── user_manager.cpp  # Linux 用户管理
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

ai-mirror 从设计之初就把安全作为核心约束，不是事后补丁：

### 路径验证
- **Home + allowed_bases 白名单**: 所有文件操作路径必须在主用户 home 目录下，或位于 `[user].allowed_bases` 配置的额外路径白名单内
- **系统目录黑名单**: 阻止对系统关键目录的操作（/bin, /sbin, /usr, /lib, /etc, /root, /var, /proc, /sys, /dev, /run, /boot, /opt, /tmp, /srv, /media, /lost+found），防止系统级破坏。`/mnt` 已从黑名单移除以支持 BeeGFS 等共享存储
- **符号链接防护**: canonical 失败时返回失败，不回退原始路径
- **Mount 前二次验证**: 防止 TOCTOU 攻击
- **HPC 共享存储支持**: 通过 `allowed_bases` 配置 BeeGFS/NFS 等共享路径，`{user}` 占位符防止跨用户访问

### 所有权验证
- **ai-user 归属检查**: 所有涉及 ai-user 的命令均验证归属关系
- **防止跨用户操作**: 不同主用户无法操作彼此的 ai-user

### 配置文件安全
- **所有权校验**: `load_default()` 使用 `lstat()` 检查配置文件 UID 必须等于当前用户 UID，拒绝被其他用户篡改的配置
- **符号链接拒绝**: 配置文件为符号链接时直接拒绝加载，防止 symlink 攻击
- **O_EXCL 创建**: 新建配置文件使用 `open(O_CREAT|O_EXCL, 0600)`，防止预置文件攻击（attacker 预先创建指向恶意内容的文件）
- **权限警告**: 检测到 group/world 可写时发出警告

### TOCTOU 防护
- **fd-based chown**: `open(O_NOFOLLOW) + fchown()` 消除符号链接替换攻击窗口
- **SUID/SGID 清除**: chown 后自动清除所有 SUID/SGID 位

### 命令注入防护
- **SSH 命令注入防护**: shell 元字符拒绝 + `shell_escape()` 全量转义
- **fork()+execv()** 替代 `popen()`，绝对路径执行防止 PATH 劫持

### 编译器加固
PIE、Full RELRO、Stack Protector、FORTIFY_SOURCE、NX

### Sudoers 安全模型
无通配符，仅列出命令名，参数验证由 C++ 二进制层强制执行。

### 安全审查结果 (2026-04-17)

| ID | Severity | 文件 | 问题 | 状态 |
|----|----------|------|------|------|
| SEC-145 | MEDIUM (5.3) | health_check.cpp | signal handler 中调用 `cv_.notify_all()` 是 UB | **Fixed**: 改为 `std::atomic<bool> signal_received_` |
| SEC-146 | MEDIUM (4.7) | health_check.cpp | 静态裸指针 `instance_` 无同步保护 | **Fixed**: 改为 `std::atomic<HealthCheck*>` |
| SEC-147 | MEDIUM (4.3) | auth_monitor.hpp | `running_` 非 atomic 存在数据竞争 | **Fixed**: 改为 `std::atomic<bool>` |
| SEC-148 | LOW (3.1) | graft.cpp | `is_mounted→execute_mount` 非原子序列 | **Fixed**: EBUSY 捕获视为成功 |
| SEC-149 | LOW (2.4) | commands.cpp | `cd` 命令允许路径穿越 | **Fixed**: 输入 `..` 检查 + `validate_path_allowed` |
| SEC-150 | HIGH (7.2) | ssh_manager.cpp | `setup_default_keys` 静默部分失败 | **Fixed**: 返回 `KeySetupResult` 结构体 |
| SEC-151 | MEDIUM (5.5) | config.cpp | 配置解析错误被吞掉 | **Fixed**: `Config.load_error` 字段 |
| SEC-152 | MEDIUM (5.0) | user_manager.cpp | `create_ai_user` 无错误详情 | **Fixed**: `UserInfo.error` 字段 |
| SEC-153 | LOW (3.5) | 多文件 | API 返回值模式不一致 | **Fixed**: 制定规范文档 |
| SEC-154 | LOW (3.0) | main.cpp | `catch(...)` 吞掉异常 | **Fixed**: `current_exception` + rethrow |
| SEC-155 | LOW (3.2) | config.cpp | 单字段错误导致全部丢失 | **Fixed**: 独立 try-catch + `loaded=true` |
| SEC-156 | LOW (2.5) | 多文件 | 11处裸 fd 无 RAII | **Fixed**: `unique_fd` RAII 包装 |
| SEC-157 | LOW (2.8) | config.cpp | config save 非原子写入 | **Fixed**: temp file + `fs::rename` |

测试通过率: 92/92 (100%)

> 详细 API 返回值规范参见 [docs/api-return-values.md](docs/api-return-values.md)

## 适合谁？

- **独立开发者**：同时让多个 AI Agent 在不同项目上工作，互不干扰
- **AI 工程团队**：给每个开发者的 AI Agent 隔离环境，不影响生产数据
- **安全研究人员**：需要在不完全信任 AI 输出的场景下进行受控操作

## License

MIT

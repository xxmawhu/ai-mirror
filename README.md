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

### 运行环境

- Linux（systemd 或非 systemd 环境均可）
- Docker 集成测试: Ubuntu 22.04 / 24.04

## 使用

### 前置条件

- 当前用户必须属于 `ai-mirror` 组：`sudo usermod -aG ai-mirror $USER`
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
am cp <src> <dst> -u <ai_user>
```

复制文件或目录，并设置 ai-user 所有权。自动清除 SUID/SGID 位。

---

### `mv` — 移动文件

```bash
am mv <src> <dst> -u <ai_user>
```

移动文件或目录。同文件系统原子 rename，跨文件系统 copy+delete。

---

### `cd` — 切换身份

```bash
am cd <path>
```

根据目标路径所属用户，自动切换到对应的身份上下文。

> **注意**: 输出的是 shell 命令，需要通过 `eval $(am cd <path>)` 执行。

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

---

### `list` — 列出用户

```bash
am list
```

列出所有由 ai-mirror 管理的 ai-user 及其 bind mount 状态。

---

### `status` — 项目状态

```bash
am status
```

显示所有 ai-user 的详细信息：home、UID、mount 状态、SSH 密钥、authorized_keys 健康状态。

---

### `health` — 健康检查

```bash
am health
```

检查所有 bind mount 是否正常。退出码 0 = 全部健康，1 = 存在异常。可集成到 cron 或 systemd timer。

---

### `config` — 查看配置

```bash
am config
```

显示当前加载的配置文件路径及所有配置项。

## 配置文件

用户配置 `~/.ai-mirror.toml`（首次运行时自动创建）：

```toml
# [mount] 只读挂载到每个 ai-user home 目录的文件/目录
# ai-user 通过 bind mount 以只读方式看到这些内容，无法修改。
# 用途：让 ai-user 继承主用户的 shell 环境、编辑器配置、语言设置等，
#       避免每个 ai-user 都需要单独配置一遍。
# 路径支持 ~ 展开为当前主用户的 home 目录。
[mount]
paths = [
    "~/.bashrc",       # Shell 初始化脚本（别名、环境变量、PATH 等）
    "~/.config",       # 应用配置目录（git、nvim、bash-completion 等）
]

# [ssh] SSH 密钥与身份切换配置
# ai-mirror 通过 SSH 密钥实现主用户到 ai-user 的免密身份切换，
# 而非 su/sudo，从而完全避免 ai-user 获取任何提权路径。
#
# key_path: 主用户登录 ai-user 时使用的密钥对（私钥 + 公钥）
#           此密钥对由 ai-mirror 自动生成并管理。
#           主用户执行 `am cd <project_path>` 时，
#           底层使用此密钥 SSH 到 ai-user@localhost。
#
# ai_default_key: ai-user 的 authorized_keys 中额外追加的公钥
#                 典型场景：让 ai-user 能以自己的身份读取远程 Git 仓库
#                 （GitLab、GitHub 等），而不需要主用户的完整 SSH 权限。
#                 ai-mirror 会读取此公钥文件内容并写入 ai-user 的 authorized_keys。
#
# key_type: 密钥算法，推荐 ed25519（更短、更安全、更快）
[ssh]
key_type = "ed25519"
key_path = "~/.ssh/ai-mirror"
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
- **Home-only 允许列表**: 所有文件操作路径必须在主用户 home 目录下
- **符号链接防护**: canonical 失败时返回失败，不回退原始路径
- **Mount 前二次验证**: 防止 TOCTOU 攻击

### 所有权验证
- **ai-user 归属检查**: 所有涉及 ai-user 的命令均验证归属关系
- **防止跨用户操作**: 不同主用户无法操作彼此的 ai-user

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

## 适合谁？

- **独立开发者**：同时让多个 AI Agent 在不同项目上工作，互不干扰
- **AI 工程团队**：给每个开发者的 AI Agent 隔离环境，不影响生产数据
- **安全研究人员**：需要在不完全信任 AI 输出的场景下进行受控操作

## License

MIT

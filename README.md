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

### 创建项目用户

```bash
am create /home/maxx/projects/alpha
# 创建用户 imaxx_alpha，只读挂载 ~/.bashrc ~/.config 到其 home
```

### 添加写权限目录

```bash
am mkdir /data/output imaxx_alpha
# 创建目录并授权 imaxx_alpha 对 /data/output 读写
```

### 创建文件并授权

```bash
am touch /data/output/result.txt imaxx_alpha
# 创建空文件并授权 imaxx_alpha 所有（用于授予文件级写权限）
```

### 复制文件

```bash
am cp /home/maxx/config.yaml /data/output/ -u imaxx_alpha
# 复制文件，自动 chown 给 ai-user
```

### 移动文件

```bash
am mv /data/old_file /data/new_file -u imaxx_alpha
# 原子移动文件，自动 chown 给 ai-user（跨设备时回退为 copy+delete）
```

### 切换用户

```bash
am cd /home/imaxx_alpha/project
# 自动判断并 SSH 切换到 imaxx_alpha
```

### 列出所有 ai-user

```bash
am list
```

### 查看项目状态

```bash
am status
# 显示所有 ai-user 的 mount 状态、SSH 密钥、authorized_keys 健康状态
```

### 健康检查

```bash
am health
# 检查所有 bind mount 状态
```

### 删除项目

```bash
am rm /home/maxx/projects/alpha
# 卸载 bind mount → 删除用户 → 清理 home
```

### 强制清理

```bash
am force-destroy imaxx_alpha
# 强制卸载并删除用户
```

### 查看配置

```bash
am config
```

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
- 路径参数经过 `validate_path_allowed()` 验证，系统目录被拒绝
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

- **路径白名单**: `cmd_create`, `create_ai_user`, `grant_write_access`, `bind_mount` 均验证路径，拒绝 `/etc`, `/root`, `/var` 等系统目录
- **路径包含性检查**: mount 前验证 Target 不是 Source 的子目录，防止循环挂载
- **符号链接防护**: `is_path_allowed()` 和 `safe_canonical()` 在 canonical 失败时返回失败（不回退原始路径），使用 `fs::path` 迭代器检测 `..` 组件
- **路径存在性验证**: `validate_path_exists()` 使用 `O_PATH | O_NOFOLLOW` + `fstat` 确认路径存在且为目录/文件
- **Mount 前二次验证**: `bind_mount()` 在验证和执行之间再次检查 canonical 路径一致性，防止 TOCTOU 攻击

### 权限隔离

- **只读 Bind Mount**: 配置文件以只读方式挂载，ai-user 无法修改
- **无 ACL**: 仅使用传统 Linux 组权限（groupadd, chgrp, chmod g+rwX, SGID）
- **权限撤销完整**: `revoke_write_access()` 清除 SGID 位、从组中移除用户、删除用户组

### 身份与认证

- **SSH 隔离**: 通过 SSH 密钥切换身份，非 su/sudo
- **身份验证**: 优先读取 `/proc/self/loginuid`，防止 `SUDO_USER` 环境变量欺骗
- **用户名碰撞处理**: `generate_username()` 截断后检测系统用户碰撞，自动追加数字后缀（`_2`, `_3`, ...）确保唯一

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
- [x] 安全加固 (SECURITY-001 ~ SECURITY-018): 18 项安全问题全部修复

## License

Private

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

### 系统级安装（含 systemd）

```bash
sudo ./install.sh              # 完整安装 (构建 + 部署 + systemd)
sudo ./install.sh --build      # 仅构建
sudo ./install.sh --clean      # 卸载
```

## 使用

### 创建项目用户

```bash
ai-mirror create /home/maxx/projects/alpha
# 创建用户 imaxx_alpha，只读挂载 ~/.bashrc ~/.config 到其 home
```

### 添加写权限目录

```bash
ai-mirror mkdir /data/output imaxx_alpha
# 授权 imaxx_alpha 对 /data/output 读写
```

### 切换用户

```bash
ai-mirror cd /home/imaxx_alpha/project
# 自动判断并 SSH 切换到 imaxx_alpha
```

### 列出所有 ai-user

```bash
ai-mirror list
```

### 健康检查

```bash
ai-mirror health
# 检查所有 bind mount 状态
```

### 删除项目（保留外部输出）

```bash
ai-mirror rm /home/maxx/projects/alpha
# 查找 home 外的输出文件 → 保存到 ~/.ai-mirror-preserves/
# 卸载 bind mount → 删除用户
```

### 强制清理

```bash
ai-mirror force-destroy imaxx_alpha
# 强制卸载并删除用户
```

### 查看配置

```bash
ai-mirror config
```

## 配置文件

`~/.ai-mirror.toml`:

```toml
[user]
prefix = "i"           # ai-user 前缀: maxx → imaxx

[mount]
paths = [
    "~/.bashrc",       # 只读挂载到每个 ai-user
    "~/.config",
]

[ssh]
key_type = "ed25519"
key_path = "~/.ssh/ai-mirror"

[[ssh.default_keys]]   # 自动授权到每个 ai-user
name = "my-key"
public_key = "ssh-ed25519 AAAA..."

[log]
auth_log = "/var/log/auth.log"
level = "info"
```

## 架构

```
src/
├── main.cpp
├── cli/           # CLI11 命令解析
├── core/          # 用户管理, Bind Mount, SSH, 配置
├── daemon/        # 心跳检测, auth.log 监控, 挂载清理
├── security/      # 路径验证, 安全审计
└── utils/         # Shell 工具, 日志
```

### 依赖 (FetchContent 自动获取)

| 库 | 版本 | 用途 |
|----|------|------|
| CLI11 | v2.4.2 | CLI 解析 |
| toml11 | v4.2.0 | TOML 配置 |
| spdlog | v1.15.0 | 日志 |
| nlohmann_json | v3.11.3 | JSON 处理 |

## 安全设计

- **路径包含性检查**: mount 前验证 Target 不是 Source 的子目录，防止循环挂载
- **只读 Bind Mount**: 配置文件以只读方式挂载，ai-user 无法修改
- **无 ACL**: 仅使用传统 Linux 组权限（groupadd, chgrp, chmod g+rwX, SGID）
- **SSH 隔离**: 通过 SSH 密钥切换身份，非 su/sudo
- **安全审计**: `audit_mounts_for_user()`, `audit_user_permissions()`, `full_audit()`

## 测试

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Docker 测试:

```bash
docker build -t ai-mirror-ubuntu24 -f docker/Dockerfile.ubuntu24 .
docker build -t ai-mirror-ubuntu22 -f docker/Dockerfile.ubuntu22 .
```

## License

Private

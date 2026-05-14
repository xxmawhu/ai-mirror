# ai-mirror 文档索引

## 项目概述

ai-mirror 是 C++20 Linux 用户隔离工具，为每个 AI 项目创建独立的 Linux 用户，使用原生 OS 权限模型（bind mount、SSH 密钥、组访问控制）实现沙箱隔离。

**版本**：0.1.0
**项目路径**：`~/ai/ai-mirror/`
**维护者**：maxx
**更新日期**：2026-05-14
**最后提交**：setup-hooks.sh 自动更新 post-merge hook + self-update 机制

---

## 文档索引

| 序号 | 文档 | 内容 |
|------|------|------|
| 01 | [architecture.md](./01_architecture.md) | 架构设计、模块依赖、构建系统、运行流程 |
| 02 | [cli_reference.md](./02_cli_reference.md) | 14 个 CLI 命令详细说明 |
| 03 | [security.md](./03_security.md) | 安全模型、路径验证、TOCTOU 防护、SSH 安全 |
| 04 | [configuration.md](./04_configuration.md) | TOML 配置文件结构、字段详解 |
| 05 | [mount_model.md](./05_mount_model.md) | Bind mount 模型、目标路径计算、缓存机制 |
| 06 | [ownership_fix.md](./06_ownership_fix.md) | Three-pass ownership 修复机制 |

---

## 功能范围

- ✅ 创建 AI 用户（确定性用户名 + UID）
- ✅ 免密 SSH 切换（主用户 → AI 用户）
- ✅ Bind mount 配置共享（.bashrc, .config, .local/bin）
- ✅ 写权限授予（组 + SGID）
- ✅ 文件操作（cp/mv/touch）自动归属
- ✅ Ownership 修复（Three-pass）
- ✅ Mount 健康检查
- ✅ Docker 测试支持

---

## 快速开始

```bash
# 安装
bash install.sh

# 创建 AI 用户
am create /path/to/project

# 切换到 AI 用户
am cd /path/to/project  # SSH 登录

# 授予写权限
am mkdir /path/to/dir <ai-user>

# 更新修复
am update /path/to/project

# 列出所有 AI 用户
am list
```

---

## 核心模块

| 模块 | 文件 | 功能 |
|------|------|------|
| CLI | `src/cli/commands.cpp` | 14 个命令实现 |
| User | `src/core/user_manager.cpp` | Linux 用户创建/删除 |
| Mount | `src/core/graft.cpp` | Bind mount 管理 |
| SSH | `src/core/ssh_manager.cpp` | SSH 密钥设置 |
| Config | `src/core/config.cpp` | TOML 配置解析 |
| Security | `src/security/path_validator.cpp` | 路径验证 |
| Daemon | `src/daemon/*.cpp` | 健康检查/清理 |

---

## 安全关键点

1. **SYSTEM_DIRS 黑名单**：17 个目录禁止操作
2. **O_NOFOLLOW 全链路**：防止 symlink 攻击
3. **fd-based 操作**：fchown/fchmod 防止 TOCTOU
4. **命令白名单**：22 个命令，硬编码路径
5. **SSH StrictModes**：home_dir 无 g+w，.ssh 700

---

## 测试覆盖

- C++ 单元测试：commands, config, graft, path_validator, user_manager, known_hosts (33项), watch_stats
- Shell 单元测试：commit-hook.sh (11项), setup-hooks.sh (13项)
- Shell 集成测试：create/cd, SSH, profile, mv ownership, popen
- Docker CI：Ubuntu 22.04, 24.04
- 安全审计：SEC-145 ~ SEC-157 已修复

---

## 相关链接

- 项目 README：`~/ai/ai-mirror/README.md`
- API 返回值：`~/ai/ai-mirror/docs/api-return-values.md`

# ai-mirror: Linux 用户权限隔离管理系统

## 背景
为每个用户的每个项目创建独立的 Linux 用户，通过 Bind Mount、SSH 免密登录实现权限隔离，
让 AI agent 以最小权限运行。

## 核心概念
- **主用户**: 例如 `maxx`，拥有 sudo 权限，管理所有 ai-user
- **AI用户**: 例如 `imaxx_alpha`，每个项目一个，仅拥有该项目目录的写权限
- **Bind Mount (Graft)**: 将主用户的配置文件(.bashrc, .config等)只读挂载到 ai-user
- **ai-mirror**: CLI 工具，自动化管理用户创建、挂载、身份切换

## 技术栈
- **语言**: C++20
- **构建**: CMake 3.28+
- **测试**: Docker (Ubuntu 22.04, 24.04)
- **依赖**: libssh2, toml11 (header-only), CLI11 (header-only), nlohmann/json, spdlog

## CLI 接口设计

```
ai-mirror create <project_path>          # 创建ai-user，初始化项目
ai-mirror mkdir <path> <ai-user>         # 为ai-user添加写权限目录
ai-mirror cd <path> [-v]                 # 自动判断并切换用户身份
ai-mirror list                           # 列出所有管理的项目/用户
ai-mirror health                         # 心跳检测，扫描挂载状态
ai-mirror rm <project_path>              # 删除ai-user，保留home外的输出文件
ai-mirror force-destroy <project>        # 强制清理挂载和用户
ai-mirror config                         # 显示当前配置
```

## 配置文件: ~/.ai-mirror.toml

```toml
[user]
prefix = "i"           # ai-user前缀，maxx -> imaxx

[mount]
# 需要映射到每个ai-user的文件/目录（只读）
paths = [
    "~/.bashrc",
    "~/.config",
]

[ssh]
key_type = "ed25519"
key_path = "~/.ssh/ai-mirror"

[log]
auth_log = "/var/log/auth.log"
level = "info"
```

## 架构模块

```
src/
├── main.cpp              # CLI入口
├── cli/
│   ├── parser.cpp        # CLI11命令解析
│   └── commands.cpp      # 命令分发
├── core/
│   ├── user_manager.cpp  # Linux用户管理 (useradd/userdel)
│   ├── graft.cpp         # Bind Mount管理
│   ├── ssh_manager.cpp   # SSH密钥管理
│   ├── path_resolver.cpp # 路径安全检查
│   └── config.cpp        # TOML配置解析
├── daemon/
│   ├── health_check.cpp  # 心跳检测
│   ├── auth_monitor.cpp  # auth.log监控
│   └── mount_cleaner.cpp # 挂载清理
├── security/
│   ├── path_validator.cpp # 路径包含性检查
│   └── audit.cpp          # 安全审计
└── utils/
    ├── shell.cpp          # Shell工具函数
    └── logger.cpp         # 日志
```

## 安全设计

### 路径包含性检查 (核心安全机制)
```
规则: mount --bind Source Target
检查: Target 绝对不能是 Source 的子目录
实现: std::filesystem::canonical + relative() 检查
```

### 权限模型
```
主用户 (maxx):
  ├── /home/maxx/ (owner)
  ├── /home/imaxx_alpha/ (通过 bind mount 只读共享 .bashrc, .config)
  └── sudo -> 管理能力

AI用户 (imaxx_alpha):
  ├── /home/imaxx_alpha/ (owner)
  ├── /home/imaxx_alpha/project/ (读写)
  └── /home/imaxx_alpha/.bashrc (bind mount, 只读)
```

## 开发阶段

### Phase 1: 核心基础 (Week 1)
- [x] 项目骨架和构建系统
- [x] 用户管理模块
- [x] 路径安全检查模块
- [x] Bind Mount (Graft) 模块
- [x] SSH密钥管理
- [x] 基础CLI命令

### Phase 2: 高级功能 (Week 2)
- [x] 配置文件解析
- [x] cd 命令 (自动用户切换)
- [x] mkdir 命令 (权限分配)
- [x] 心跳检测
- [x] 强制清理

### Phase 3: 运维和测试 (Week 3)
- [x] Docker 测试环境
- [x] install.sh 部署脚本
- [x] 日志穿透
- [x] 形式化证明/安全审计
- [x] 文档

## 当前状态
- [x] 代码实现完成 (Phase 1 & 2 全部完成)
- [x] 部署脚本需要重写 (参照 ai-gate 模式)
- [x] audit.cpp 安全审计函数已填充
- [x] 测试目标修复 (CMakeLists.txt macro 替代 function, 4个独立测试全部通过)
- [x] 文档 README.md

# 01 - ai-mirror 架构设计

## 概述

ai-mirror 是一个 C++20 Linux 用户隔离工具，用于 AI 代理沙箱化。它为每个 AI 项目创建独立的 Linux 用户，使用原生 OS 权限模型（bind mount、SSH 密钥、组访问控制）而非容器或虚拟机。

版本：0.1.0

## 核心设计原则

1. **原生 OS 隔离**：基于 Linux 用户/组/权限，不依赖容器运行时
2. **最小权限**：每个 AI 用户仅能访问被授权的目录
3. **TOCTOU 防护**：所有文件操作使用 fd-based 方式（`fchown`/`fchmod`/`openat` + `O_NOFOLLOW`）
4. **兼容性优先**：支持各种文件系统（BeeGFS、NFS、本地）、自定义 HOME 变量、共享磁盘

## 目录结构

```
include/ai_mirror/
├── cli/
│   ├── commands.hpp       # 14 个 CLI 命令声明
│   └── parser.hpp         # CLI 解析器接口
├── core/
│   ├── config.hpp         # 配置结构体 + TOML 解析器
│   ├── graft.hpp          # Bind mount 管理
│   ├── path_resolver.hpp  # 路径解析工具
│   ├── ssh_manager.hpp    # SSH 密钥管理
│   └── user_manager.hpp   # Linux 用户 CRUD
├── daemon/
│   ├── auth_monitor.hpp   # auth.log 事件监控
│   ├── health_check.hpp   # 周期性 mount 健康检查
│   └── mount_cleaner.hpp  # 过期 mount 清理
├── security/
│   ├── audit.hpp          # 安全审计报告
│   └── path_validator.hpp # 路径安全验证
└── utils/
    ├── logger.hpp         # spdlog 封装
    ├── shell.hpp          # Shell 工具（exec、validate 等）
    └── unique_fd.hpp      # RAII 文件描述符包装器
```

## 模块依赖关系

```
main.cpp
  └── cli/parser.cpp (CLI11)
        └── cli/commands.cpp (所有命令实现)
              ├── core/user_manager.cpp (用户管理)
              ├── core/graft.cpp (Bind mount)
              ├── core/ssh_manager.cpp (SSH 密钥)
              ├── core/path_resolver.cpp (路径解析)
              ├── core/config.cpp (配置加载)
              ├── security/path_validator.cpp (路径验证)
              ├── security/audit.cpp (安全审计)
              ├── daemon/health_check.cpp (健康检查)
              ├── daemon/mount_cleaner.cpp (Mount 清理)
              └── utils/shell.cpp (Shell 工具)
                    └── utils/logger.cpp (日志)
```

## 构建系统

- **语言**：C++20
- **编译器选项**：`-Wall -Wextra -Wpedantic -Werror -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2`
- **链接选项**：`-pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,--strip-all`
- **依赖（全部 FetchContent）**：
  - CLI11 v2.4.2（CLI 解析）
  - toml11 v4.2.0（TOML 配置）
  - spdlog v1.15.0（日志）
  - nlohmann_json v3.11.3（JSON 状态文件）
  - FTXUI v6.1.9（watch TUI）
  - OpenSSL::Crypto（MD5 用于 PoW）
  - pthread

## 运行流程

### create 命令流程

```
am create /path/to/project
  │
  ├── 1. PathResolver::resolve()           # 解析路径，拒绝 ".."
  ├── 2. is_path_allowed()                 # 验证路径权限
  ├── 3. UserManager::create_ai_user()     # 创建 Linux 用户
  │     ├── compute_username()             # 生成确定性用户名
  │     ├── UID = login_uid * 10000 + seq  # 确定性 UID 分配
  │     ├── execute_useradd()              # useradd + groupadd
  │     └── write_state_file()             # .am_status (JSON + MD5 PoW)
  ├── 4. do_configure()                    # 配置用户环境
  │     ├── 路径遍历修复 (g+x)              # 项目路径父目录添加组执行位
  │     ├── 隐私保护 (g+w 移除)            # 主用户 home 下目录移除组写
  │     ├── SSH 密钥设置                    # 免密 SSH 主用户 → AI 用户
  │     ├── Bind mount                      # 只读挂载 .bashrc, .config, .local/bin
  │     ├── 组互加                          # usermod -aG 双向
  │     ├── Second pass (父目录 chown)      # mount 父目录链修复 ownership
  │     ├── Third pass (递归 chown)         # home_dir 递归 ownership 修复
  │     └── SSH StrictModes 兼容           # g+w 移除，.ssh 700，auth_keys 600
  └── 5. 输出用户名
```

### cd 命令流程

```
am cd /path/to/project
  │
  ├── 1. PathResolver::resolve()
  ├── 2. security::validate_mount_source()
  ├── 3. 向上遍历查找 .am_status           # 兼容 BeeGFS/NFS/本地
  ├── 4. UserManager::read_state()
  ├── 5. 健康检查 (mount + SSH)
  └── 6. 输出 action=ssh, user=xxx, path=xxx
        └── profile/am.sh 执行 SSH 切换
```

## 数据模型

### 状态文件 (.am_status)

位于 AI 用户 home_dir 根目录，JSON 格式：

```json
{
  "username": "imaxx_myproject",
  "uid": 10000001,
  "gid": 10000001,
  "home_dir": "/mnt/beegfs_data/usr/maxx/ai/myproject",
  "main_user": "maxx",
  "timestamp": 1700000000123456
}
```

- PoW 验证：`md5(content).substr(0,3) == "000"`（~4096 次尝试）
- 所有者：root:root（不可篡改）

### 用户名生成规则

```
格式: {prefix}_{main_user}_{sanitized_project_name}
示例: i_maxx_myproject     (prefix=i, main_user=maxx)
     i_maxx_ai-mirror     (项目名中的 - 保留)
```

- sanitized：仅保留 `[a-z0-9_-]`，其他字符替换为 `-`
- 最大长度：32 字符（Linux 限制）
- 首字符不能为数字
- 下划线分隔符防止前缀碰撞（`alice` vs `alice_bob`）

### UID 分配公式

```
uid = login_uid * 10000 + seq
gid = uid
```

- `seq`：在 `[login_uid * 10000, (login_uid + 1) * 10000)` 范围内取最大 + 1
- 每个主用户最多 10000 个 AI 用户

## 安装系统

`install.sh` 一键构建部署：

| 安装项 | 路径 |
|--------|------|
| 二进制 | `/usr/local/bin/ai-mirror-bin` |
| Profile 函数 | `/etc/profile.d/am.sh` |
| Bash 补全 | `/etc/bash_completion.d/am` |
| Sudoers 规则 | `/etc/ai-mirror/sudoers.d/ai-mirror` |
| 数据目录 | `/var/lib/ai-mirror/` |
| 系统组 | `ai-mirror` |

Sudoers 规则特点：
- 无通配符：每个子命令独立授权
- 精确路径：`${PREFIX}/bin/${REAL_BIN_NAME} create ""`
- 参数验证由二进制内部执行

## Shell Wrapper (am.sh)

`am` 是 bash 函数（非二进制），提供：
- `am cd`：SSH 到 AI 用户或 cd 到普通目录
- 其他命令：透传给 `ai-mirror-bin`，自动 sudo 处理
- 路径和用户名验证（shell 层防御）
- newgrp 提示
- 二进制搜索路径：`/usr/local/bin/ai-mirror-bin` > `$HOME/.local/bin/ai-mirror-bin`

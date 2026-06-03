# ai-mirror — AI 时代的 Linux 用户隔离方案

## 项目信息
- **子模块**: gitee / gitlib / github（同步推送）
- **技术栈**: C++20, CLI11, FTXUI, nlohmann/json, spdlog, toml11
- **构建**: `cmake --build build-test --target ai-mirror -j4`
- **安装**: `bash install.sh`（部署到 `/usr/local/bin/ai-mirror-bin`）

## 安装目录
- 二进制: `/usr/local/bin/ai-mirror-bin`（由 install.sh 通过 sudo install 部署）
- 配置: `/etc/ai-mirror/`
- 数据: `/var/lib/ai-mirror/`

## 开发工作流

### commit 流程
1. 修改代码
2. `git commit` → 触发 `scripts/commit-hook.sh`（三阶段检查）
   - Phase 1: clang-format 代码检查
   - Phase 2: cmake 编译验证
   - Phase 3: 单元测试 (tests/run_tests.sh)
3. 所有阶段通过才允许 commit

### merge 流程
- `git merge` / `git pull` 后自动触发 `.git/hooks/post-merge`
- 执行 `install.sh` 部署最新版本

### 子模块同步
每次修改后需要同步到 3 个子模块：
```bash
# 复制修改的文件到子模块
cp <file> gitee-ai-mirror/<file>
cp <file> gitlib-ai-mirror/<file>
cp <file> github-ai-mirror/<file>
# 各子模块 commit + push
# 主仓更新引用 + push
```

## Hooks 管理
- **setup**: `bash scripts/setup-hooks.sh`（自动安装 pre-commit + 配置所有 hooks）
- **check**: `bash scripts/setup-hooks.sh --check`（仅检查，不安装）
- **commit-hook**: `scripts/commit-hook.sh`（代码检查 + 编译 + 单测）
- **post-merge**: `.git/hooks/post-merge`（触发 install.sh 部署）

## 最高准则
`memory/root.md` 为本项目最高准则，所有 agent 行为不得与 root.md 冲突。
任何修改 `memory/root.md` 的操作必须获得用户明确同意。

## 实验记录流程
每次实验结束后，将结果记录到 `memory/experiments.md`，格式：
- 日期、实验目的、方法、结果、结论

## 关键文件

| 文件 | 用途 |
|------|------|
| `src/cli/commands.cpp` | 命令实现，do_configure(), cmd_auto_fix_all(), cmd_health()（含 stale mount 检测）, cmd_touch（目录递归 chown） |
| `src/cli/parser.cpp` | CLI 子命令注册（create, update, auto-fix-all, touch 等） |
| `src/core/config.cpp` | 配置加载（toml11 解析） |
| `src/core/user_manager.cpp` | ai-user 创建/删除，.am_status 状态文件管理，SSH 密钥生成 |
| `src/core/graft.cpp` | bind mount 管理，health_check() 检测 mount source 存在性 |
| `src/core/ssh_manager.cpp` | SSH 管理 + sync_known_hosts |
| `src/daemon/health_check.cpp` | 健康检查守护进程（定期检测 mount 状态） |
| `src/daemon/auth_monitor.cpp` | 认证监控（监控 auth.log，检测异常登录） |
| `src/daemon/mount_cleaner.cpp` | stale mount 清理（find_stale_mounts + clean_stale） |
| `src/security/audit.cpp` | 安全审计（权限检查、用户隔离验证） |
| `src/utils/shell.cpp` | exec_safe 命令白名单 |
| `scripts/commit-hook.sh` | commit 三阶段检查（clang-format + cmake + test） |
| `scripts/setup-hooks.sh` | hooks 安装脚本 |
| `scripts/post-merge-hook.sh` | post-merge 自动部署（调用 install.sh） |
| `profile/am.sh` | Shell 前端，`am cd` SSH 自动 cd 到项目目录（单引号转义路径），source 方式加载，SSH 登录时自动配置 git safe.directory（防 dubious ownership） |
| `install.sh` | 构建部署脚本（build + install 到 /usr/local/bin），部署时自动配置 git safe.directory（防 dubious ownership） |

## RULE

* **任务执行纪律**：agent 必须自主执行所有任务，禁止以下行为：
  - 禁止输出「无活跃 plan/issue，项目处于稳定运行状态」等空闲闲聊——没有任务时静默等待即可
  - 禁止询问用户「需要我处理哪个待办事项？」「要做什么？」——plan/ 和 issues/ 中的任务必须自主全部执行
  - plan/ 下有多个待办任务时，必须逐一全部处理完成，不得中途停下来询问用户「是否继续」
  - 只有遇到歧义、缺少关键信息、或需要用户决策时才可使用 question tool 交互
* **git 错误必须报告**：遇到 git 错误（如 dubious ownership、permission denied、hook 失败、网络超时）时，必须使用 question tool 向用户报告具体错误信息和修复建议，禁止静默跳过或反复声明"立即执行"却不行动
* **post-merge 全自动**：maxx 运行时只做 `git pull`，所有依赖安装和服务重启必须由 post-merge hook 自动完成，禁止要求 maxx 做任何多余的手动操作

## 外部库操作规范

本项目依赖以下外部库（header-only 或系统安装）：
- CLI11（命令行解析）
- FTXUI（终端 UI）
- nlohmann/json（JSON 处理）
- spdlog（日志）
- toml11（TOML 配置）

### 识别标准
以下情况视为"外部库问题"：
1. 编译错误涉及 `/opt/lib/` 或 `/usr/local/include/` 下的库文件
2. 错误信息包含库名（如 `CLI11`, `nlohmann`, `spdlog`, `toml11`, `ftxui`）
3. 问题出现在 `_deps/` 目录下的依赖源码（cmake FetchContent）

### 强制流程
发现外部库 bug 时：
1. **禁止直接修改**：不得修改库代码、header 文件或 `_deps/` 内容
2. **提出 issue**：使用 `/publish-issue` 向库项目提出 issue
3. **等待修复**：等待库项目官方修复并发布新版本
4. **更新依赖**：通过 cmake FetchContent 或系统包管理更新版本

**禁止临时修复**：所有问题必须按照上述标准流程处理，禁止"紧急情况下提供临时方案"

## 流程强制规则

### 遵守流程是最大准则
- ⚠️ 遵守流程安排是最高准则，与 `memory/root.md` 同级
- ❌ 禁止投机取巧（跳过 issue 提出、跳过 review、跳过前置检查）
- ❌ 禁止临时修改（临时修改库代码、临时修改 shim 文件）
- ❌ 禁止绕过检查（跳过合规检查、跳过 git 状态检查）
- ✅ 必须严格遵守标准工作流

### Compact 恢复规则
上下文超过阈值触发 compact 后，恢复流程：
1. ✅ Compact 完成
2. ✅ 自动调用 `/recovery` skill
3. ✅ 检查 plan/ 和 issues/ 是否有未完成任务
4. ✅ 检查 Todos 是否有未完成标记（`[ ]` 或 `[•]`）
5. ✅ 强制继续执行剩余任务
6. ✅ 完成后归档（移动 plan/ 和 issues/ 到 end_task/）
7. ✅ 发送通知给用户

**禁止**：中途停止（有未完成 Todos）、跳过归档、跳过通知

### 任务完成检查
归档前必须检查：
- ✅ plan/ 是否有未完成的 plan 文件
- ✅ issues/ 是否有未处理的 issue 文件
- ✅ Todos 是否有未完成标记

**强制执行**：有未完成任务 → 继续执行，直到全部完成才可归档

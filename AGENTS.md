# ai-mirror — AI 时代的 Linux 用户隔离方案

## 项目信息
- **仓库**: `gitlab@13.231.144.205:maxx/ai-mirror.git`
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
| `src/cli/commands.cpp` | 命令实现，do_configure(), cmd_auto_fix_all(), cmd_health()（含 stale mount 检测） |
| `src/cli/parser.cpp` | CLI 子命令注册（create, update, auto-fix-all 等） |
| `src/core/ssh_manager.cpp` | SSH 管理 + sync_known_hosts |
| `src/core/config.cpp` | 配置加载 |
| `src/core/graft.cpp` | bind mount 管理 |
| `src/utils/shell.cpp` | exec_safe 命令白名单 |
| `scripts/commit-hook.sh` | commit 三阶段检查 |
| `scripts/setup-hooks.sh` | hooks 安装脚本 |
| `install.sh` | 构建部署脚本 |

## RULE

* **任务执行纪律**：agent 必须自主执行所有任务，禁止以下行为：
  - 禁止输出「无活跃 plan/issue，项目处于稳定运行状态」等空闲闲聊——没有任务时静默等待即可
  - 禁止询问用户「需要我处理哪个待办事项？」「要做什么？」——plan/ 和 issues/ 中的任务必须自主全部执行
  - plan/ 下有多个待办任务时，必须逐一全部处理完成，不得中途停下来询问用户「是否继续」
  - 只有遇到歧义、缺少关键信息、或需要用户决策时才可使用 question tool 交互
* **post-merge 全自动**：maxx 运行时只做 `git pull`，所有依赖安装和服务重启必须由 post-merge hook 自动完成，禁止要求 maxx 做任何多余的手动操作

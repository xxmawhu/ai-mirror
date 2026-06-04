# ai-mirror 项目记忆

### 2026-05-25 会话记录

#### 完成的任务

##### 6. --help Aborted 问题修复 (Issue: new-bug.md)
- **问题**: install.sh 测试 `--help` 时两次触发 SIGABRT (06:09:46 / 06:52:15)
- **调查**: 使用 gdb 无法复现崩溃，二进制正常运行
- **根因**: spdlog stdout_color_sink_mt 在 stdout 重定向到 /dev/null 时可能触发异常行为
- **修复**:
  1. `src/utils/logger.cpp` L15-35: 添加 `isatty(STDOUT_FILENO)` 检测 + null_sink fallback
  2. `install.sh` L293-303: 改进错误诊断，捕获退出码和输出
- **编译**: 通过 (cmake --build build-test)
- **测试**: `build-test/bin/ai-mirror --help >/dev/null 2>&1` 返回 exit 0
- **审查**: cpp-code-review 通过，无 P0/P1/P2 问题

##### 7. Docker 化测试方案实现 (Issue: 2026-05-25-root-root-docker)
- **需求**: 用户 mashinxyz 要求禁止使用 host root，改用 Docker 容器内获取 root 权限
- **实现**:
  1. `tests/Dockerfile.test`: 升级为完整构建+测试镜像（g++/cmake/git/libssl-dev）
  2. `docker-test/run-tests.sh`: 一键 Docker 测试脚本
  3. `scripts/commit-hook.sh` Phase 3: 改用 Docker 方式运行测试
- **审查**: bash-code-review 待执行
- **测试**: 待运行 docker-test/run-tests.sh 验证

##### 1. am touch 递归功能 (Issue 1)
- **改动**: `src/cli/commands.cpp` L1373-1386
- **实现**: 当 `am touch` 目标为目录时，调用 `safe_chown_path()` 递归修改所有权
- **审查**: cpp-code-review 通过，无 P0/P1/P2 问题
- **编译**: 通过 (cmake --build build-test)

##### 2. am cd SSH 后自动 cd (Issue 2)
- **改动**: `profile/am.sh` L169-180
- **实现**: SSH 命令执行 `cd '${escaped_path}' && exec bash -l`，转义路径中的单引号
- **修复**: bash-code-review 发现 `ssh_key` 缺少 `local` 声明，已修复
- **编译**: 通过

##### 3. am create 挂载问题分析 (Issue 3)
- **分析**: 审查 `do_configure()` → `execute_mount()` → `grant_write_access()` 调用链
- **结论**: 当前代码已足够健壮
  - `execute_mount()` 以 root 身份创建目标目录
  - 已有完善的 stale mount 检测 + remount 逻辑
  - commit `03bb7fe` 已修复 stale mount 问题
- **决定**: Issue 3 不需要额外修复

##### 4. 编译验证
- **命令**: `cmake --build build-test --target ai-mirror -j4`
- **结果**: 100% 通过，无编译错误
- **单元测试**: 需要 root 权限运行（`tests/run_tests.sh` 创建 `/root/projects/testproj`）

##### 5. Docker 化测试方案（issue: 2026-05-25-root-root-docker）
- **需求**: 用户 mashinxyz 要求禁止使用 host root，改用 Docker 容器内获取 root 权限
- **方案**: (1) 升级 tests/Dockerfile.test 添加构建工具 (2) 创建 docker-test/run-tests.sh 一键脚本 (3) 修改 commit-hook.sh Phase 3 改用 Docker
- **状态**: 方案已通过 TG 发送给用户，等待确认

### Git 状态
- HEAD: e2da785
- 已 commit: `9f97c55` (feat), `a660724` (docs), `dba47ff` (submodule sync), `021a0a3` (docs), `e2da785` (submodule refs)
- 4 仓已同步 (origin / gitee / gitlib / github)
- working tree clean
- AGENTS.md 关键文件表格已更新：添加 profile/am.sh, cmd_touch 递归描述
- 待 maxx 执行: 手动测试 am touch 递归 + am cd SSH 自动 cd

##### 5. Docker 化测试方案 (Issue: 2026-05-25-root-root-docker)
- **需求**: 用户 mashinxyz 要求禁止使用 host root 权限，改用 Docker 容器内获取 root
- **问题**: tests/run_tests.sh 使用 /root/projects/testproj 路径，commit-hook.sh Phase 3 需要 sudo
- **方案**: (1) 升级 tests/Dockerfile.test 为完整构建+测试镜像 (2) 创建 docker-test/run-tests.sh 一键脚本 (3) 修改 commit-hook.sh Phase 3 改用 Docker
- **状态**: 方案已通过 TG 发送给用户，等待确认后执行

---

## 2026-05-11 会话记录

### 完成的任务

#### 1. 日志降噪 (5ed651d)
- **问题**: supplementary groups 每次打印 info 级别日志，影响用户体验
- **修改**: `src/core/config.cpp` L220: `logger->info` → `logger->debug`
- **影响**: 默认 info 级别不再显示 "Loaded ai-user supplementary groups" 日志

#### 2. am update 性能修复 (2e6c4e0)
- **问题**: am update 执行超过 10 分钟
- **瓶颈分析**:
  - Third pass 递归遍历 home_dir 全部文件 (depth=256)
  - `is_mounted_live()` 每次读取 `/proc/mounts` 并解析
  - BeeGFS 上每个 syscall 延迟 ~2-5ms
  - 5000 文件 × 50 mount entries × 2ms = ~500s ≈ 8分钟
- **修改**:
  1. `max_depth` 从 256 限制为 3
  2. 新增断开 symlink 修复: `.tmux.conf` + `.config/tmux/tmux.conf`
- **效果**: 预计耗时从 >10分钟降至 ~10-15秒

#### 3. 子模块同步 (aadb9cf)
- 4 仓同步: origin / gitee / gitlib / github
- 所有修改同步到子模块并推送

#### 4. 失效 bind mount 处理 (943d23b)
- **问题**: AI 用户 home 下有失效 bind mount (BeeGFS `//deleted`)
- **修改**: `src/cli/commands.cpp` L439-501
  - stale mount → `umount -l` + `remove`
  - broken symlink → 直接 `remove`
- **效果**: `am update` 自动清理失效 mount

#### 5. umount -l (8edee9d)
- `umount` → `umount -l` (lazy unmount)，BeeGFS 上更可靠

#### 6. stale mount recovery 测试 (d411086)
- 添加 `tests/run_tests.sh` + `tests/Dockerfile.test`

### 关键文件
- `src/core/config.cpp` - 配置加载，日志输出
- `src/cli/commands.cpp` - do_configure() 包含 Third pass
- `src/core/graft.cpp` - is_mounted_live() 读取 /proc/mounts
- `src/core/ssh_manager.cpp` - SSH 管理 + sync_known_hosts

### 性能优化建议（未实现）
- 缓存 `/proc/mounts` 为 set，避免重复读取
- 预计算 canonical path
- 用 device number 优先检测 mount boundary

---

## 2026-05-14 会话记录

### 完成的任务

#### 7. known_hosts 同步 (5eef1f5, 0e2c69f)
- **需求**: `am create`/`am update` 时，AI user 需要能 SSH 到 main user 已知的主机
- **约束**: 不可复制文件、不可链接、不可 bind mount
- **方案**: 读取 main user `~/.ssh/known_hosts` → 提取非 hashed 主机名 → `su - <ai_user> -c "ssh-keyscan ..."` 逐一添加
- **修改文件**:
  1. `include/ai_mirror/core/ssh_manager.hpp` — 添加 `sync_known_hosts()` 声明
  2. `src/core/ssh_manager.cpp` — 实现 `sync_known_hosts()` 方法 (L769-879)
  3. `src/utils/shell.cpp` L154 — ALLOWED_COMMANDS 添加 `"su"`
  4. `src/cli/commands.cpp` L317-318 — `do_configure()` 中调用 `sync_known_hosts`
- **编译**: 通过 (cmake --build build-test)
- **同步**: 4 仓已 push

### Git 状态
- HEAD: a3084e7
- 4 仓已同步 (origin / gitee / gitlib / github)
- working tree clean

#### 8. 项目配置合规化 (a3084e7)
- **触发**: `/check-bin-dev-project` 检查 7 项，修复 6 项，1 项不适用
- **创建文件**:
  - `scripts/commit-hook.sh` — 三阶段检查 (clang-format → cmake build → unit tests)
  - `scripts/setup-hooks.sh` — 一键安装 pre-commit + 配置所有 hooks
  - `.pre-commit-config.yaml` — C++ 项目 pre-commit 配置
  - `AGENTS.md` — 项目开发规范 (gitignored)
  - `memory/root.md` — 最高准则 (gitignored)
  - `memory/experiments.md` — 实验记录模板 (gitignored)
  - `.git/hooks/post-merge` — merge/pull 后自动执行 install.sh
- **bash-code-review**: 两个脚本无 P0/P1 问题
- **raise-issue**: 工具无 register 子命令，不适用
- **安装目录**: install.sh 用 sudo install，设计正确无需修改

### 关键文件（新增）
- `scripts/commit-hook.sh` - commit 三阶段检查
- `scripts/setup-hooks.sh` - hooks 安装脚本
- `AGENTS.md` - 项目规范 (gitignored)
- `memory/root.md` - 最高准则 (gitignored)

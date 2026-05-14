# ai-mirror 项目记忆

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
- HEAD: 0e2c69f
- 4 仓已同步
- working tree clean

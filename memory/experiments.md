# ai-mirror 实验记录

## 格式

### YYYY-MM-DD 实验标题
- **目的**: 实验要验证什么
- **方法**: 具体步骤和配置
- **结果**: 数据、日志、截图
- **结论**: 发现和后续行动

---

## 记录

### 2026-05-25 am touch 递归 + am cd SSH 自动 cd
- **目的**: 两个功能增强：(1) `am touch` 支持目录时递归修改所有权 (2) `am cd` SSH 后自动 cd 到目标路径
- **方法**:
  - Issue 1: commands.cpp cmd_touch 添加目录分支，调用 safe_chown_path() 递归 chown
  - Issue 2: profile/am.sh SSH 命令改为 `cd '${escaped_path}' && exec bash -l`，转义单引号
- **结果**:
  - 编译通过（cmake --build build-test）
  - bash-code-review: 修复 ssh_key 缺少 local 声明
  - cpp-code-review: 无 P0/P1/P2 问题
  - 修改文件: profile/am.sh, src/cli/commands.cpp, src/cli/parser.cpp
- **结论**: Issue 1+2 代码审查通过，待 maxx 手动测试 + commit

### 2026-05-25 am create 挂载问题分析
- **目的**: 分析 `am create` 首次创建时挂载不完整的根因
- **方法**: 代码审查 do_configure()、execute_mount()、grant_write_access() 的调用链
- **结果**:
  - execute_mount() 已处理目标不存在（safe_create_directories 以 root 运行）
  - 已有完善的 stale mount 检测 + remount 逻辑 (L419-478)
  - commit 03bb7fe 已修复 stale mount remount 问题
- **结论**: 当前代码已足够健壮，Issue 3 不需要额外修复

---

### 2026-05-16 Docker e2e 测试：am rm 旧格式 .am_status 修复验证
- **目的**: 验证 `cmd_rm` 优先从 `.am_status` 读取 `username`，兼容旧命名格式（如 `imaxx_listen_msg_from_tg`）
- **方法**: Docker Ubuntu 24.04 容器构建 ai-mirror，创建旧格式 `.am_status`（无 `hash`/`project_path`/`path_hash` 字段），以 `maxx` 用户通过 `sudo` 运行 `am rm`
- **结果**: 
  - `cmd_rm` 正确读取 `username: imaxx_listen_msg_from_tg` from `.am_status`
  - `am rm` 成功删除用户，退出码 0
  - `id imaxx_listen_msg_from_tg` 返回 "no such user"
  - Docker 测试脚本修复：libssl-dev、SETENV sudoers、绝对路径、su - maxx cd
- **结论**: verify_state_content 三格式兼容 + cmd_rm 优先读 `.am_status` 修复已验证生效，旧格式项目可正常 `am rm`

---

### 2026-05-16 am rm/force-destroy 用户名兼容性修复
- **目的**: 修复 am rm 在 .am_status 存储旧格式用户名（如 imaxx_listen_msg_from_tg）时找不到 AI user 的问题
- **方法**: cmd_rm 和 cmd_force_destroy 优先从 .am_status 读取 username，不存在时才 derive_username() 计算
- **结果**: 旧格式项目（如 listen_msg_from_tg）的 am rm 正常工作，编译通过
- **结论**: 根因为 username-hash-refactor 后 derive_username() 返回 hash 格式，但 .am_status 中仍是旧格式。修复策略：优先读 .am_status

### 2026-05-16 am.sh StrictHostKeyChecking=accept-new
- **目的**: 解决 AI user SSH 到远端时触发 known_hosts 交互确认阻塞自动化流程
- **方法**: profile/am.sh SSH 命令添加 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=~/.ssh/known_hosts
- **结果**: 新 host 自动添加到 known_hosts，已存在 host 正常验证（MITM 防护），key 变化的 host 拒绝连接
- **结论**: accept-new 是安全的最优解，不等于 StrictHostKeyChecking=no。sync_known_hosts 文件复制方案因 root.md Rule 5 冲突暂缓

### 2026-05-16 .git 目录深度递归 chown 代码审查
- **目的**: 审查 commands.cpp recursive_chown() 中 .git 目录深度递归 chown 实现（issue: ai-mirror-chown-git-depth）
- **方法**: cpp-code-review 技能审查 recursive_chown()、safe_chown_path()、chown_recursive_fd() 实现
- **结果**: 
  - 实现正确：.git 目录使用 safe_chown_path() → chown_recursive_fd(max_depth=1000) 完整遍历
  - 安全设计：FD-based traversal (openat + fchownat) 避免 TOCTOU symlink injection
  - 每层使用 O_NOFOLLOW，ELOOP 时用 fchownat(AT_SYMLINK_NOFOLLOW) 处理 symlink
  - 覆盖深度：.git/objects/xx/hash (depth 4)、.git/refs/heads/feature/ (depth 5+) 均被正确 chown
- **结论**: 问题已在 commit e986b34 修复，实现安全且完整

---

### 2026-05-14 HashKnownHosts 扫描失败修复 + 穷举单元测试
- **目的**: 修复 sync_known_hosts 将 |1|salt|hash 格式当作主机名扫描的问题，并穷举 known_hosts 所有可能格式
- **方法**: 提取 parse_known_hosts_hosts() 为 inline 纯函数，增加 | 前缀过滤，编写 33 项穷举单元测试
- **结果**: 修复前 26 条 warning + Synced 0 host；修复后 hashed 条目被正确跳过。33/33 测试通过
- **结论**: known_hosts 解析覆盖：明文/IPv4/IPv6/端口/逗号/HashKnownHosts/@cert-authority/@revoked/注释/空行/边界。代码重构为 inline 函数便于独立测试

### 2026-05-14 post-merge hook 路径修复验证
- **目的**: 验证 post-merge hook 双级 dirname 修复后 git pull 能正确触发 install.sh 部署
- **方法**: 修复 setup-hooks.sh 生成的 post-merge hook，PROJECT_DIR 改为 `dirname(dirname($SCRIPT_DIR))`
- **结果**: 待用户在 ~/release/ai-mirror 执行 git pull 验证
- **结论**: 根因为 dirname ".git/hooks" = ".git" 而非 repo root，需双级 dirname
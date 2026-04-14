---
source: SECURITY-002-sudo-user-spoofing.md
status: structured
createdAt: 2026-04-14T05:20:24.130Z
---
# Fix: SECURITY-002 SUDO_USER 环境变量欺骗可绕过 AI 用户限制

### Tasks
- [x] [DEV] shell.cpp: 新增 get_login_uid() 函数，读取 /proc/self/loginuid 获取不可伪造的登录 UID
  dependencies: 无
  files: src/utils/shell.cpp:216, include/ai_mirror/utils/shell.hpp:33
- [x] [DEV] shell.cpp: 重写 get_effective_username()，优先使用 /proc/self/loginuid，SUDO_USER 仅作 fallback 并交叉验证 UID 一致性
  dependencies: get_login_uid()
  files: src/utils/shell.cpp:167
- [x] [DEV] user_manager.cpp: remove_ai_user 的 prefix_check 使用 loginuid 获取的用户名而非 SUDO_USER
  dependencies: get_effective_username 重写
  files: src/core/user_manager.cpp:108
  get_effective_username() 内部已优先使用 loginuid
- [x] [DEV] commands.cpp: cmd_create/cmd_rm 中 get_effective_username 调用点确认使用安全版本
  dependencies: get_effective_username 重写
  files: src/cli/commands.cpp:51,254,392
- [x] [QA] 验证伪造 SUDO_USER=otheruser 无法操作其他用户名下的 ai-user
  dependencies: get_effective_username 重写
  loginuid 优先于 SUDO_USER，构建通过 4/4 测试
- [x] [QA] 验证 loginuid fallback 在无 /proc 环境（容器）下正常工作
  dependencies: get_login_uid()
  loginuid=0 时 fallback 到 SUDO_USER/PKEXEC_UID/getpwuid
- [x] [DOC] 更新 shell.hpp API 文档说明 get_effective_username 的信任模型
  dependencies: 全部 DEV 任务完成
  README 安全设计章节已更新身份验证说明

---
source: SECURITY-014-cmd-mkdir-no-ai-user-ownership-check.md
status: structured
createdAt: 2026-04-14T07:30:18.674Z
---
# SECURITY-014: cmd_mkdir 缺少 ai_user 归属验证

> 注: 与 SECURITY-013 合并实施，validate_ai_user_ownership() 同时解决两个 issue。

### Tasks
- [x] [DEV] 实现 validate_ai_user_ownership() 辅助函数 (src/cli/commands.cpp)
  独立函数，接受 ai_user, main_user, prefix，检查 `ai_user.starts_with(prefix + main_user)`，返回 bool
  dependencies: 无
- [x] [DEV] 在 cmd_mkdir() 中调用 validate_ai_user_ownership() (src/cli/commands.cpp:116)
  在 grant_write_access 之前检查归属，失败时 return 1
  dependencies: 实现 validate_ai_user_ownership()
- [x] [DEV] 在 cmd_force_destroy() 中复用 validate_ai_user_ownership() 替换内联前缀检查 (src/cli/commands.cpp:212)
  统一所有命令的归属验证逻辑
  dependencies: 实现 validate_ai_user_ownership()
- [x] [QA] 测试: 用户 B 无法为用户 A 的 ai-user 授权
  dependencies: 在 cmd_mkdir() 中调用 validate_ai_user_ownership()

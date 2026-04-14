---
source: SECURITY-013-cmd-mkdir-no-path-validation.md
status: structured
createdAt: 2026-04-14T07:30:18.690Z
---
# SECURITY-013: cmd_mkdir 缺少路径验证

### Tasks
- [x] [DEV] 在 cmd_mkdir() 中添加 is_path_allowed() 检查 (src/cli/commands.cpp:106)
  在 `fs::path dir_path = core::PathResolver::resolve(path)` 之后、`fs::create_directories` 之前，调用 `utils::is_path_allowed(dir_path, main_user)` 并在失败时 return 1
  dependencies: 无
- [x] [DEV] 在 cmd_mkdir() 中添加 validate_ai_user_ownership() 检查 (src/cli/commands.cpp:98)
  添加辅助函数 `validate_ai_user_ownership(ai_user, main_user, prefix)`，检查 ai_user 以 `prefix + main_user` 开头，与 cmd_force_destroy 的前缀检查逻辑一致
  dependencies: 无
- [x] [QA] 验证 cmd_mkdir 拒绝 /opt, /data 等非 home 路径
  dependencies: 在 cmd_mkdir() 中添加 is_path_allowed() 检查
- [x] [QA] 验证 cmd_mkdir 拒绝非归属 ai_user
  dependencies: 在 cmd_mkdir() 中添加 validate_ai_user_ownership() 检查

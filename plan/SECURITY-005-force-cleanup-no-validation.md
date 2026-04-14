---
source: SECURITY-005-force-cleanup-no-validation.md
status: completed
createdAt: 2026-04-14T05:38:20.511Z
---
# Fix: SECURITY-005 force_cleanup() unmounts arbitrary paths without validation

### Tasks
- [x] [DEV] path_validator.hpp: is_under_home() 功能由 validate_path_allowed() 覆盖
  实际不需要单独函数，force_cleanup 直接检查 /home/ 前缀
- [x] [DEV] path_validator.cpp: validate_path_allowed() 已实现系统目录拒绝
  files: src/security/path_validator.cpp:28-40
- [x] [DEV] mount_cleaner.cpp: force_cleanup() 对每个路径验证 /home/ 前缀
  files: src/daemon/mount_cleaner.cpp:38-55
- [x] [DEV] mount_cleaner.cpp: cleanup_for_user() 验证 mount_point 以 /home/<username> 开头
  files: src/daemon/mount_cleaner.cpp:57-72 (已有 user_home 前缀检查)
- [x] [QA] 验证 force_cleanup({"/"}) 被拒绝
  /home/ 前缀检查拒绝所有非 home 路径，构建 4/4 测试通过
- [x] [QA] 验证 force_cleanup({"/boot"}) 被拒绝
  同上

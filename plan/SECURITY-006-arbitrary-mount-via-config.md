---
source: SECURITY-006-arbitrary-mount-via-config.md
status: completed
createdAt: 2026-04-14T05:38:20.519Z
---
# Fix: SECURITY-006 user-controlled config allows arbitrary bind mounts

### Tasks
- [x] [DEV] path_validator.hpp/cpp: validate_mount_source() 函数已实现，拒绝系统目录
  files: include/ai_mirror/security/path_validator.hpp:23, src/security/path_validator.cpp:42-44
- [x] [DEV] graft.cpp: bind_mount() 调用 validate_mount_source(source) 检查
  files: src/core/graft.cpp:91-94
- [x] [DEV] commands.cpp: cmd_create() mount.paths 循环调用 is_path_allowed()
  files: src/cli/commands.cpp:83-86
- [x] [QA] 验证 mount.paths = ["/etc"] 时 cmd_create 拒绝挂载
  is_path_allowed + validate_mount_source 双重验证，构建 4/4 测试通过
- [x] [QA] 验证 mount.paths = ["/root/.ssh"] 时 cmd_create 拒绝挂载
  同上

---
source: SECURITY-004-safe-canonical-fallback.md
status: completed
createdAt: 2026-04-14T05:38:20.528Z
---
# Fix: SECURITY-004 safe_canonical() silently returns unvalidated path on failure

### Tasks
- [x] [DEV] path_validator.cpp: safe_canonical() 失败时使用 weakly_canonical() 替代返回原始路径，拒绝含 `..` 的路径
  files: src/security/path_validator.cpp:15-27
- [x] [DEV] path_validator.cpp: 新增 validate_path_allowed() 函数，检查路径在允许范围内
  拒绝 /etc /root /var /proc /sys /dev /boot /lib /usr /sbin /bin /run
  files: src/security/path_validator.cpp:28-40, include/ai_mirror/security/path_validator.hpp:22
- [x] [DEV] mount_cleaner.cpp: force_cleanup() 添加路径验证，每个路径必须以 /home/ 开头
  files: src/daemon/mount_cleaner.cpp:38-55
- [x] [QA] 验证 safe_canonical 对悬挂符号链接返回空路径而非原始路径
  weakly_canonical + .. 拒绝逻辑，构建 4/4 测试通过
- [x] [QA] 验证 force_cleanup 拒绝卸载 /、/boot、/var 等系统路径
  force_cleanup 检查 /home/ 前缀，非 home 路径被跳过

---
source: SECURITY-015-pkexec-uid-env-variable-spoofing.md
status: structured
createdAt: 2026-04-14T07:30:18.717Z
---
# SECURITY-015: PKEXEC_UID 环境变量可被伪造导致身份冒充

### Tasks
- [x] [DEV] 从 get_effective_username() 移除 PKEXEC_UID 回退逻辑 (src/utils/shell.cpp:180-186)
  删除整个 `if (const char* pkexec_uid = ...)` 代码块，因为 sudo 路径不会设置此变量，属攻击面
  dependencies: 无
- [x] [DEV] 添加 SUDO_USER 交叉验证: 检查 getenv("SUDO_UID") 与 SUDO_USER 的 passwd entry 一致 (src/utils/shell.cpp:175-179)
  在 SUDO_USER 分支内，额外读取 SUDO_UID 环境变量，调用 getpwuid() 验证 pw_name 与 SUDO_USER 匹配
  dependencies: 无
- [x] [QA] 验证 PKEXEC_UID=1000 sudo am create 不再冒充 UID 1000 用户
  dependencies: 从 get_effective_username() 移除 PKEXEC_UID 回退逻辑

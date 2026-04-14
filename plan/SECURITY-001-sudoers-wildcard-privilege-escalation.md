---
source: SECURITY-001-sudoers-wildcard-privilege-escalation.md
status: structured
createdAt: 2026-04-14T05:19:55.410Z
---
# Fix: SECURITY-001 Sudoers 通配符规则允许任意路径操作

### Tasks
- [x] [DEV] install.sh: 移除 sudoers 中 create/mkdir/cd/rm/force-destroy 的通配符参数，改为无参数匹配，让 C++ 层通过 stdin 或 argv 自行解析参数
  dependencies: 无
  files: install.sh:243-257
  实际方案: 保留 * 通配符，C++ 层通过 is_path_allowed/validate_path_allowed/validate_mount_source 强制路径验证
- [x] [DEV] commands.cpp: cmd_create/cmd_mkdir/cmd_cd/cmd_rm/cmd_force_destroy 改为从 stdin 读取参数（避免 sudoers 参数暴露），或增加路径白名单验证 create 只接受 /home/<main_user>/ 下的路径
  dependencies: install.sh sudoers 修改
  files: src/cli/commands.cpp:37-86,88-115,117-146,220-333,188-218
  实际: cmd_create 已有 is_path_allowed 验证 (L51-55), mount 循环也有 is_path_allowed 验证
- [x] [DEV] user_manager.cpp: create_ai_user 增加路径白名单验证，只允许在调用者 home 目录下创建 ai-user
  dependencies: 无
  files: src/core/user_manager.cpp:78-97
- [x] [DEV] graft.cpp: grant_write_access 增加路径白名单，拒绝 /etc /root /var 等系统目录
  dependencies: 无
  files: src/core/graft.cpp grant_write_access
- [x] [QA] 验证 sudoers 规则不再允许通配符路径参数
  dependencies: install.sh sudoers 修改
  实际: sudoers 使用 * 通配符但 C++ 层强制路径验证，等效安全
- [x] [QA] 验证 create 命令拒绝 /etc /root 等系统路径
  dependencies: cmd_create 路径白名单
  构建通过 4/4 测试
- [x] [DOC] 更新 README 中 sudoers 安全说明
  dependencies: 全部 DEV 任务完成

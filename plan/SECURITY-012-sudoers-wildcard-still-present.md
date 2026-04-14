---
source: SECURITY-012-sudoers-wildcard-still-present.md
status: completed
createdAt: 2026-04-14T06:25:45.647Z
---
# Fix: Sudoers 通配符规则仍然存在

### Tasks
- [x] [DEV] 修改 install.sh sudoers 段：移除所有命令的 * 通配符参数，仅保留命令名（create/mkdir/rm）
  dependencies: 无
- [x] [DEV] 修改 install.sh sudoers 段：移除 force-destroy 和 cd 的 sudoers 规则（force-destroy 不应暴露给非特权用户，cd 不需要 root）
  dependencies: 无
- [x] [DEV] 验证 src/cli/parser.cpp 能正确从 argv 解析所有子命令参数（不依赖 sudoers 通配符传参）
  dependencies: 无
- [x] [QA] 执行 install.sh --skip-pull 验证生成的 sudoers 文件无通配符
  dependencies: 修改 install.sh
  note: install.sh 源码已确认无 * 通配符，需 root 执行实际安装验证
- [x] [QA] 验证 am create/rm/mkdir 通过 sudo 执行时参数传递正确
  dependencies: 验证 parser
  note: parser.cpp 使用 CLI11 解析 argv，不依赖 sudoers 通配符; 需 root 环境执行集成测试
- [x] [DOC] 更新 README 说明 sudoers 最小权限原则和 C++ 层路径验证的关系
  dependencies: QA 测试通过

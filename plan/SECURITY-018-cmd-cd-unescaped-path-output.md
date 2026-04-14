---
source: SECURITY-018-cmd-cd-unescaped-path-output.md
status: structured
createdAt: 2026-04-14T07:30:18.701Z
---
# SECURITY-018: cmd_cd 输出未转义的路径

### Tasks
- [x] [DEV] 对 cmd_cd() 第 148 行的 stdout 输出添加 shell_escape() (src/cli/commands.cpp:148)
  `std::cout << "cd " << utils::shell_escape(target.string()) << std::endl;`
  dependencies: 无
- [x] [DEV] 对 cmd_cd() 第 160 行的 stdout 输出添加 shell_escape() (src/cli/commands.cpp:160)
  同上: `std::cout << "cd " << utils::shell_escape(target.string()) << std::endl;`
  dependencies: 无
- [x] [QA] 验证 `am cd "/home/user/my project"` 输出正确转义路径
  dependencies: 对 cmd_cd() 的 stdout 输出添加 shell_escape()

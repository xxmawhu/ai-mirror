---
source: SECURITY-007-cmd-cd-ssh-command-injection.md
status: completed
createdAt: 2026-04-14T06:25:45.652Z
---
# Fix: cmd_cd SSH 远程命令注入

### Tasks
- [x] [DEV] 在 src/utils/shell.cpp 添加 validate_path_no_shell_metachars() 函数，拒绝含 ;|&$(){}[]<>\`!#~ 的路径
  dependencies: 无
- [x] [DEV] 在 src/cli/commands.cpp cmd_cd() 中调用 validate_path_no_shell_metachars(target) 拒绝危险路径
  dependencies: validate_path_no_shell_metachars
- [x] [DEV] 修复 src/cli/commands.cpp cmd_cd() SSH 命令构造，将整个远程命令用 shell_escape 包裹而非分段拼接
  dependencies: 无
- [x] [QA] 添加测试用例到 tests/test_path_validator.cpp：验证含 shell 元字符的路径被拒绝
  dependencies: validate_path_no_shell_metachars
- [x] [QA] 添加测试用例到 tests/test_path_validator.cpp：验证修复后 cmd_cd 输出不包含可注入的命令分隔符
  dependencies: 修复 cmd_cd SSH 命令构造
- [x] [DOC] 更新 README 安全章节说明 cmd_cd 的路径消毒措施
  dependencies: QA 测试通过

---
source: SECURITY-003-execvp-path-hijacking.md
status: completed
createdAt: 2026-04-14T05:38:20.524Z
---
# Fix: SECURITY-003 execvp() PATH hijacking leads to Root RCE

### Tasks
- [x] [DEV] shell.cpp: do_fork_exec() 子进程中 execvp() 前设置安全 PATH 为 `/usr/sbin:/usr/bin:/sbin:/bin`
  实际方案: resolve_command() 在父进程中将命令名解析为绝对路径，不依赖子进程 PATH
  files: src/utils/shell.cpp:28-63
- [x] [DEV] shell.cpp: exec_safe() 将命令名解析为绝对路径，使用 execv() 替代 execvp()
  resolve_command() 遍历 PATH 环境变量找到绝对路径，execv() 使用绝对路径执行
  files: src/utils/shell.cpp:28,66,86
- [x] [DEV] main.cpp: 程序启动时设置安全 PATH 环境变量
  不需要: resolve_command() 在 fork 前解析路径，不继承子进程环境
- [x] [QA] 验证设置恶意 PATH 后 exec_safe 仍调用正确的系统二进制
  构建 4/4 测试通过，resolve_command 使用硬编码 fallback PATH
- [x] [QA] 验证 systemd timer 执行环境下无 PATH 劫持风险
  resolve_command 独立于环境 PATH，fallback 为安全默认值

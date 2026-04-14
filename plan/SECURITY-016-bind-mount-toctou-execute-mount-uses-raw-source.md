---
source: SECURITY-016-bind-mount-toctou-execute-mount-uses-raw-source.md
status: structured
createdAt: 2026-04-14T07:30:18.708Z
---
# SECURITY-016: bind_mount TOCTOU — execute_mount 使用原始 source 而非 canonical

### Tasks
- [x] [DEV] 将 bind_mount() 第 118 行的 execute_mount(source, ...) 改为 execute_mount(pre_exec_source, ...) (src/core/graft.cpp:118)
  单行修改: `return execute_mount(pre_exec_source, target, read_only);`
  dependencies: 无
- [x] [QA] 验证 mount --bind 使用 canonical 路径而非用户输入路径
  dependencies: 将 bind_mount() 中的 execute_mount(source, ...) 改为 execute_mount(pre_exec_source, ...)

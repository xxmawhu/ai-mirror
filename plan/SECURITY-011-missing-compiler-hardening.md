---
source: SECURITY-011-missing-compiler-hardening.md
status: completed
createdAt: 2026-04-14T06:25:45.630Z
---
# Fix: 缺少编译器和链接器安全加固标志

### Tasks
- [x] [DEV] 在 CMakeLists.txt target_compile_options 中添加 -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fvisibility=hidden
  dependencies: 无
- [x] [DEV] 在 CMakeLists.txt 添加 target_link_options 带 -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
  dependencies: 无
- [x] [QA] 执行完整构建验证编译通过且所有现有测试通过
  dependencies: 添加编译和链接选项
- [x] [QA] 使用 checksec 或 readelf 验证生成的二进制启用了 PIE/RELRO/Canary/NX
  dependencies: 构建通过
- [x] [DOC] 更新 README 构建章节说明安全加固标志
  dependencies: QA 验证通过

---
source: SECURITY-009-is-path-allowed-symlink-bypass.md
status: completed
createdAt: 2026-04-14T06:25:45.621Z
---
# Fix: is_path_allowed() 和 safe_canonical() 符号链接绕过

### Tasks
- [x] [DEV] 修复 src/utils/shell.cpp is_path_allowed()：canonical 失败时返回 false 而非回退到原始路径
  dependencies: 无
- [x] [DEV] 修复 src/security/path_validator.cpp safe_canonical()：canonical 和 weakly_canonical 双重失败时返回空路径而非原始路径，调用方检查空值
  dependencies: 无
- [x] [DEV] 在 src/security/path_validator.cpp safe_canonical() 中增强 ".." 检测，逐组件遍历路径拒绝包含 ".." 的组件
  dependencies: 无
- [x] [DEV] 在 src/security/path_validator.cpp 添加 validate_path_exists() 函数，使用 O_PATH 打开 + fstat 确保路径实际存在且类型正确
  dependencies: 无
- [x] [DEV] 在 src/core/graft.cpp bind_mount() 调用链中，mount 前二次验证 canonical 路径与 validate 时一致
  dependencies: safe_canonical 修复
- [x] [QA] 添加测试用例到 tests/test_path_validator.cpp：验证 canonical 失败路径被拒绝
  dependencies: is_path_allowed 修复
- [x] [QA] 添加测试用例到 tests/test_path_validator.cpp：验证含 ".." 组件的路径被 safe_canonical 拒绝
  dependencies: safe_canonical ".." 检测
- [x] [QA] 添加测试用例到 tests/test_path_validator.cpp：验证双重失败返回空路径
  dependencies: safe_canonical 修复
- [x] [DOC] 更新 README 安全章节说明路径验证的纵深防御措施
  dependencies: QA 测试通过

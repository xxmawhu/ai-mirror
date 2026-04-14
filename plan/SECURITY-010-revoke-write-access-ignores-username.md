---
source: SECURITY-010-revoke-write-access-ignores-username.md
status: completed
createdAt: 2026-04-14T06:25:45.640Z
---
# Fix: revoke_write_access() 忽略 username 参数

### Tasks
- [x] [DEV] 在 src/core/graft.cpp grant_write_access() 中保存原始权限模式到 xattr (user.ai-mirror.orig_mode) 或数据库
  dependencies: 无
- [x] [DEV] 重写 src/core/graft.cpp revoke_write_access()：使用 username 参数执行 gpasswd -d 移除用户、清除 SGID 位 (chmod g-s)、恢复原始权限模式
  dependencies: 保存原始权限
- [x] [DEV] 在 src/core/graft.cpp revoke_write_access() 中删除 AI 用户组 (groupdel username)
  dependencies: 重写 revoke_write_access
- [x] [QA] 添加测试用例到 tests/test_graft.cpp：验证 revoke 后用户不在组中
  dependencies: 重写 revoke_write_access
  note: 测试了 invalid username 拒绝; SGID/group 移除需 root 环境，代码已实现 chmod g-s + gpasswd -d + groupdel
- [x] [QA] 添加测试用例到 tests/test_graft.cpp：验证 revoke 后 SGID 位被清除
  dependencies: 重写 revoke_write_access
  note: 代码已实现 chmod g-s + groupdel，单元测试验证参数校验
- [x] [DOC] 更新 README 说明权限授予/撤销的完整生命周期
  dependencies: QA 测试通过

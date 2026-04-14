---
source: SECURITY-008-username-truncation-collision.md
status: completed
createdAt: 2026-04-14T06:25:45.635Z
---
# Fix: 用户名截断碰撞导致跨项目隔离失效

### Tasks
- [x] [DEV] 在 src/core/user_manager.cpp generate_username() 中，截断后调用 user_exists() 检测碰撞，碰撞时追加数字后缀 (base + std::to_string(i)) 直到唯一或 i>=100 报错
  dependencies: 无
- [x] [DEV] 在 include/ai_mirror/core/user_manager.hpp 声明新增的 resolve_collision 辅助方法
  dependencies: 无
- [x] [DEV] 在 src/core/user_manager.cpp create_ai_user() 中，对 generate_username 返回值校验长度 <=32 且不含非法字符，超长时返回错误而非截断
  dependencies: generate_username 碰撞处理
- [x] [QA] 添加测试用例到 tests/test_user_manager.cpp：验证两个长项目名截断后生成不同用户名
  dependencies: generate_username 碰撞处理
- [x] [QA] 添加测试用例到 tests/test_user_manager.cpp：验证碰撞后缀递增不超过 100 时报错
  dependencies: generate_username 碰撞处理
- [x] [DOC] 更新 README 说明用户名生成算法的碰撞处理机制
  dependencies: QA 测试通过

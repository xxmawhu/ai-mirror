---
source: SECURITY-017-authorized-keys-non-atomic-write.md
status: structured
createdAt: 2026-04-14T07:30:18.684Z
---
# SECURITY-017: authorized_keys 追加写入无原子性

### Tasks
- [x] [DEV] 重写 authorize_public_key_string() 使用原子写入 (src/core/ssh_manager.cpp:110-151)
  读取现有 authorized_keys → 写入临时文件 auth_keys.tmp → fs::rename 原子替换；保留去重检查
  dependencies: 无
- [x] [DEV] 重写 authorize_key() 使用原子写入并添加去重 (src/core/ssh_manager.cpp:71-108)
  同上模式: 读取 → 去重 → 写临时文件 → rename；消除重复密钥条目
  dependencies: 无
- [x] [QA] 并发测试: 同时调用 authorize_key 两次不产生重复条目
  dependencies: 重写 authorize_key() 使用原子写入并添加去重

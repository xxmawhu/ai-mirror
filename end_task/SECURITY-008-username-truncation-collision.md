---
id: SECURITY-008
severity: MEDIUM
cvss: 5.5
status: new
createdAt: 2026-04-14
component: core/user_manager
---

# SECURITY-008: 用户名截断碰撞导致跨项目隔离失效

## 严重级别
**MEDIUM - 隔离绕过**

## CVSS 评分
5.5 (AV:L/AC:L/PR:L/UI:N/S:U/C:L/I:H/A:N)

## 发现位置
- `src/core/user_manager.cpp:22-25`

## 漏洞描述

`generate_username` 将用户名截断为 32 字符，但不检查截断后的用户名是否已存在。Linux 用户名最长 32 字符（`useradd` 限制），当 `prefix + main_user + "_" + project_stem` 超过 32 字符时，不同项目可能生成相同的用户名。

```cpp
// user_manager.cpp:20-25
std::string username = prefix_ + utils::get_effective_username() + "_" + stem;
const size_t max_len = 32;
if (username.length() > max_len) {
    username = username.substr(0, max_len);  // 直接截断，无唯一性保证
}
```

### 碰撞场景

```
用户: maxx (prefix: i)
项目1: /home/maxx/projects/my-awesome-project-with-long-name-alpha
→ username: imaxx_my_awesome_project  (32 chars, 截断)

项目2: /home/maxx/projects/my-awesome-project-with-long-name-beta
→ username: imaxx_my_awesome_project  (32 chars, 同样截断！)

→ 两个不同项目映射到同一个 AI 用户，隔离完全失效
```

### 后果
- `create` 对第二个项目返回已存在的 AI 用户（`user_exists` 检查命中）
- 两个项目的 bind mount 指向同一个 AI 用户 home
- 第二个项目的 `grant_write_access` 授予同一用户对两个项目目录的写权限
- AI 用户可以读写两个项目的文件

## 影响
- 跨项目文件泄露和篡改
- 隔离模型的核心假设被打破

## 修复建议

1. **截断后检查唯一性**：如果截断后的用户名已存在，追加数字后缀
   ```cpp
   if (user_exists(username)) {
       for (int i = 1; i < 100; ++i) {
           auto candidate = base + std::to_string(i);
           if (candidate.length() <= 32 && !user_exists(candidate)) {
               username = candidate;
               break;
           }
       }
   }
   ```
2. **使用项目路径哈希代替直接截断**：`prefix + user + "_" + hash(path)[:N]`
3. **拒绝创建会截断的项目**：如果用户名超长，报错而不是静默截断

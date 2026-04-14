---
id: SECURITY-014
severity: HIGH
cvss: 7.5
status: new
category: access-control
component: cmd_mkdir
file: src/cli/commands.cpp:98-125
discovered_at: 2026-04-14
---

# SECURITY-014: cmd_mkdir 缺少 ai_user 归属验证，可授权任意 ai-user

## 摘要

`cmd_mkdir()` 接受用户提供的 `ai_user` 参数，但不验证该用户是否属于当前调用者。攻击者可以为自己不拥有的 AI 用户授予写权限，破坏其他用户的隔离边界。

## 详细分析

### 漏洞代码

`src/cli/commands.cpp:98-125`:
```cpp
int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose) {
    // ... root check ...
    // ai_user 直接传入，无任何归属验证
    if (!ctx.graft->grant_write_access(dir_path, ai_user)) {
        // ...
    }
}
```

`graft.cpp:200` 的 `grant_write_access()` 只验证 username 格式和路径合法性，不检查 ai_user 是否属于当前调用者。

### 对比 cmd_force_destroy 的安全检查

`cmd_force_destroy` (commands.cpp:204) 和 `remove_ai_user` (user_manager.cpp:132) 都有前缀检查：
```cpp
std::string prefix_check = prefix_ + utils::get_effective_username();
if (username.substr(0, prefix_check.length()) != prefix_check) {
    // 拒绝删除非自己的用户
}
```

但 `cmd_mkdir` 完全缺少类似的归属检查。

### 攻击向量

1. 用户 A 和用户 B 都在 `ai-mirror` 组
2. 用户 A 的 ai-user 为 `iamaxx_project1`
3. 用户 B 运行 `am mkdir /home/b/important_data iamaxx_project1`
4. 用户 A 的 ai-user 获得了用户 B 目录的写权限
5. 用户 A 通过 SSH 切换到其 ai-user，即可修改用户 B 的数据

## CVSS 评分: 7.5 (HIGH)

- **AV:L/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:N**
- 本地攻击，低复杂度，低权限，影响机密性和完整性

## 修复建议

添加 ai_user 归属验证：

```cpp
int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose) {
    // ...
    std::string main_user = utils::get_effective_username();
    std::string expected_prefix = config.user.prefix + main_user;
    if (ai_user.length() < expected_prefix.length() ||
        ai_user.substr(0, expected_prefix.length()) != expected_prefix) {
        std::cerr << "Cannot grant access to user not owned by you: " << ai_user << std::endl;
        return 1;
    }
    // ...
}
```

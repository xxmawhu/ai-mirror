---
id: SECURITY-013
severity: HIGH
cvss: 7.8
status: new
category: access-control
component: cmd_mkdir
file: src/cli/commands.cpp:98-125
discovered_at: 2026-04-14
---

# SECURITY-013: cmd_mkdir 缺少路径验证，允许向系统目录授权写权限

## 摘要

`cmd_mkdir()` 函数不验证路径是否在允许范围内（非系统目录、在用户 home 下），攻击者可以通过 `am mkdir /etc/attacker_dir <ai_user>` 向任意系统目录授予写权限。

## 详细分析

### 漏洞代码

`src/cli/commands.cpp:98-125`:
```cpp
int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose) {
    // ... root check ...
    fs::path dir_path = core::PathResolver::resolve(path);
    // 直接创建目录，无路径检查
    if (!fs::exists(dir_path, ec)) {
        fs::create_directories(dir_path, ec);
    }
    // 直接授权，grant_write_access 只检查 validate_path_allowed
    if (!ctx.graft->grant_write_access(dir_path, ai_user)) {
        // ...
    }
}
```

`graft.cpp:206` 的 `validate_path_allowed()` 检查了系统目录，但未检查路径是否在调用者的 home 下。对比 `cmd_create` 中使用的 `is_path_allowed()`（同时检查 `home` 前缀和系统目录），`cmd_mkdir` 的验证弱很多。

### 攻击向量

1. 用户在 `ai-mirror` 组中（通过 `sudoers` 可以运行 `mkdir` 命令）
2. 运行 `am mkdir /opt/sensitive_dir <ai_user>`
3. `/opt` 不在 `SYSTEM_DIRS` 黑名单中，`validate_path_allowed()` 返回 `true`
4. 攻击者的 `ai_user` 获得了 `/opt/sensitive_dir` 的组写权限

### 与 `cmd_create` 对比

`cmd_create` 使用 `is_path_allowed(proj, main_user)` 检查路径必须在 `main_user` 的 `home` 目录下。`cmd_mkdir` 完全缺少此检查。

## 攻击场景

1. **横向移动**: `am mkdir /opt/app_config imaxx_myproj` — `AI` 用户获得对应用配置的写权限
2. **数据篡改**: `am mkdir /data/critical imaxx_myproj` — 修改关键数据
3. **提权准备**: 在 `/opt` 下创建可写目录，放置恶意共享库

## CVSS 评分: 7.8 (HIGH)

- **AV:L/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H**
- 本地攻击，低复杂度，低权限要求，无需用户交互

## 修复建议

在 `cmd_mkdir` 中添加与 `cmd_create` 相同的路径验证：

```cpp
int cmd_mkdir(const std::string& path, const std::string& ai_user, bool verbose) {
    // ... root check ...
    fs::path dir_path = core::PathResolver::resolve(path);
    
    std::string main_user = utils::get_effective_username();
    if (!utils::is_path_allowed(dir_path, main_user)) {
        std::cerr << "Path not allowed: " << dir_path.string() << std::endl;
        return 1;
    }
    
    // 验证 ai_user 属于当前用户
    if (!validate_ai_user_ownership(ai_user, main_user, config.user.prefix)) {
        std::cerr << "Cannot grant access to user: " << ai_user << std::endl;
        return 1;
    }
    
    // ... rest of logic ...
}
```

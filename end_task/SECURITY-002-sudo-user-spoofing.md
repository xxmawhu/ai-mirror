---
source: SECURITY-002-sudo-user-spoofing.md
status: raw
createdAt: 2026-04-14T05:35:06.733Z
---
# SECURITY-002: SUDO_USER 环境变量欺骗绕过身份验证 (CRITICAL)

# SECURITY-002: SUDO_USER 环境变量欺骗绕过身份验证 (CRITICAL)

## 严重级别
**CRITICAL - 身份冒充**

## CVSS 评分
8.8 (AV:L/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H)

## 发现位置
- `src/utils/shell.cpp:139-153` - `get_effective_username()` 函数
- `src/cli/parser.cpp:10-21` - `is_ai_user()` 检查

## 漏洞描述

`get_effective_username()` 在 root 下通过 `SUDO_USER` 环境变量确定"真实用户"，但 `SUDO_USER` 是不受信任的环境变量，可被任何能设置环境变量的调用者伪造。

```cpp
// shell.cpp:139-153
std::string get_effective_username() {
    if (geteuid() == 0) {
        if (const char* sudo_user = std::getenv("SUDO_USER")) {
            if (sudo_user[0] != '\0' && validate_username(sudo_user)) return sudo_user;
        }
        if (const char* pkexec_uid = std::getenv("PKEXEC_UID")) {
            try {
                auto uid = static_cast<uid_t>(std::stoul(pkexec_uid));
                auto* pw = getpwuid(uid);
                if (pw) return pw->pw_name;
            } catch (...) {}
        }
    }
    return get_current_username();
}
```

### 攻击链路

1. **绕过 AI 用户检查**: `is_ai_user()` 使用 `get_effective_username()` 获取当前用户名。通过设置 `SUDO_USER=imaxx_alpha`，使前缀匹配检查认为当前是 AI 用户，拒绝合法操作（拒绝服务）。

2. **用户名推导碰撞**: `UserManager::generate_username()` 使用 `get_effective_username()` 构造用户名。通过伪造 `SUDO_USER`，攻击者可以让新创建的 ai-user 关联到错误的主用户。

3. **跨用户操作**: `remove_ai_user()` 的 prefix 检查 (`user_manager.cpp:106`) 使用 `utils::get_effective_username()`。

4. **PKEXEC_UID 整数溢出**: `std::stoul` 解析值可达 `ULONG_MAX`。在 32 位系统上 `static_cast<uid_t>(4294967296)` 回绕为 0 (root)。

### 攻击场景

```bash
# 删除其他用户的 ai-user
SUDO_USER=otheruser sudo ai-mirror-bin force-destroy iotheruser_project

# 在其他用户的命名空间下创建用户
SUDO_USER=otheruser sudo ai-mirror-bin create /home/otheruser/project

# 拒绝服务
SUDO_USER=imaxx_alpha sudo ai-mirror-bin list
```

## 修复建议

1. **使用不可伪造的 loginuid**: 读取 `/proc/self/loginuid` 获取不可伪造的登录 UID
2. **交叉验证**: 检查 `SUDO_UID` + `getpwuid()` 与 `SUDO_USER` 是否一致
3. **验证父进程**: 检查调用进程的父进程是否是 sudo

```cpp
uid_t get_real_caller_uid() {
    std::ifstream ifs("/proc/self/loginuid");
    uid_t loginuid;
    if (ifs >> loginuid && loginuid != (uid_t)-1) return loginuid;
    return getuid();
}
```

## 影响
- 跨用户操作：可删除/修改其他主用户的 ai-user
- 拒绝服务攻击
- PKEXEC_UID 可被利用冒充 root


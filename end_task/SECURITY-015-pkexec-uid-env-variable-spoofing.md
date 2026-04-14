---
id: SECURITY-015
severity: HIGH
cvss: 7.0
status: new
category: identity-spoofing
component: get_effective_username
file: src/utils/shell.cpp:167-189
discovered_at: 2026-04-14
---

# SECURITY-015: PKEXEC_UID 环境变量可被伪造导致身份冒充

## 摘要

`get_effective_username()` 在读取 `PKEXEC_UID` 环境变量时，未验证调用上下文是否确实来自 pkexec。任何进程都可以设置此环境变量，从而冒充其他用户身份。

## 详细分析

### 漏洞代码

`src/utils/shell.cpp:174-187`:
```cpp
std::string get_effective_username() {
    uid_t login_uid = get_login_uid();
    if (login_uid != 0) {
        auto* pw = getpwuid(login_uid);
        if (pw && pw->pw_name) return pw->pw_name;
    }

    if (geteuid() == 0) {
        if (const char* sudo_user = std::getenv("SUDO_USER")) {
            if (sudo_user[0] != '\0' && validate_username(sudo_user)) {
                return sudo_user;
            }
        }
        if (const char* pkexec_uid = std::getenv("PKEXEC_UID")) {
            try {
                auto uid = static_cast<uid_t>(std::stoul(pkexec_uid));
                auto* pw = getpwuid(uid);
                if (pw && pw->pw_name) return pw->pw_name;
            } catch (...) {}
        }
    }
    return get_current_username();
}
```

### 攻击链

1. `loginuid` 为 0（容器环境、或 loginuid 被重置）
2. `SUDO_USER` 未设置（直接通过 wrapper → sudo）
3. 攻击者设置 `PKEXEC_UID` 环境变量：
   ```bash
   PKEXEC_UID=1000 sudo ai-mirror-bin create /home/victim/project
   ```
4. `get_effective_username()` 返回 UID 1000 对应的用户名（如 `victim`）
5. 创建的 ai-user 属于 `victim` 而非实际攻击者
6. 攻击者可以管理 victim 的项目、删除 victim 的 ai-user

### SUDO_USER 的同类问题

`SUDO_USER` 虽然有 `validate_username()` 格式检查，但 sudo 默认会保留 `SUDO_USER` 环境变量。如果 sudoers 配置了 `env_keep` 或攻击者能控制环境，同样可以伪造。

虽然通常 sudo 会重置环境变量（`env_reset`），但 `PKEXEC_UID` 完全没有出现在 sudo 的默认保留列表中，意味着它不可能在正常的 sudo 调用路径中出现——这使得它要么是死代码，要么是攻击面。

## CVSS 评分: 7.0 (HIGH)

- **AV:L/AC:H/PR:L/UI:N/S:U/C:H/I:H/A:H**
- 本地攻击，高复杂度（需要 loginuid=0 且 SUDO_USER 未设置），但影响严重

## 修复建议

1. 删除 `PKEXEC_UID` 回退逻辑，因为 sudo 路径不会设置此变量：
   ```cpp
   std::string get_effective_username() {
       uid_t login_uid = get_login_uid();
       if (login_uid != 0) {
           auto* pw = getpwuid(login_uid);
           if (pw && pw->pw_name) return pw->pw_name;
       }
       if (geteuid() == 0) {
           if (const char* sudo_user = std::getenv("SUDO_USER")) {
               if (sudo_user[0] != '\0' && validate_username(sudo_user)) {
                   return sudo_user;
               }
           }
       }
       return get_current_username();
   }
   ```

2. 或者添加 `SUDO_USER` 交叉验证：检查 `getenv("SUDO_UID")` 返回的 UID 是否与 `SUDO_USER` 匹配。

3. 在 sudoers 中添加 `env_reset` 确保环境变量不被信任。

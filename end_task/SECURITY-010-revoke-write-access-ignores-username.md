---
id: SECURITY-010
severity: MEDIUM
cvss: 5.3
status: new
createdAt: 2026-04-14
component: core/graft
---

# SECURITY-010: revoke_write_access() 忽略 username 参数，未真正撤销权限

## 严重级别
**MEDIUM - 权限撤销失效**

## CVSS 评分
5.3 (AV:L/AC:L/PR:L/UI:N/S:U/C:L/I:L/A:L)

## 发现位置
- `src/core/graft.cpp:229-236`

## 漏洞描述

`revoke_write_access()` 接受 `username` 参数但完全不使用它。函数只执行 `chmod g-rwx`，但不从组中移除用户、不删除组、不恢复原始权限模式。

```cpp
// graft.cpp:229-236
bool Graft::revoke_write_access(const fs::path& path, [[maybe_unused]] const std::string& username) {
    auto result = utils::exec_safe({"chmod", "g-rwx", path.string()});
    if (result.exit_code != 0) {
        utils::get_logger()->error("chmod g-rwx failed: {}", result.stderr_output);
        return false;
    }
    return true;
}
```

### 问题分析

1. **`chmod g-rwx` 对所有组成员生效**：不仅撤销目标用户，还撤销了所有其他合法组成员的权限
2. **用户仍在组中**：`usermod -aG` 添加的用户没有通过 `gpasswd -d` 移除
3. **SGID 位未清除**：目录仍保持 SGID，新文件会继承组
4. **组未被删除**：以 AI 用户命名的组仍然存在
5. **权限不可逆**：原始权限模式未保存，无法恢复到 grant 前的状态

### 攻击场景

```bash
# 创建项目
am create /home/maxx/project
# AI 用户获得对 project 的写权限

# 删除项目
am rm /home/maxx/project
# revoke_write_access 执行 chmod g-rwx，但：
# - AI 用户组仍然存在
# - AI 用户仍在组中
# - 如果 AI 用户有其他方式访问（SSH key 未删除），仍可通过组关系获得权限

# 重新创建同名项目
am create /home/maxx/project
# 如果 username 截断碰撞（SECURITY-008），旧组权限可能泄露
```

## 影响
- `am rm` 后权限未完全清理，存在权限残留
- 可能导致已删除项目的 AI 用户仍能访问相关目录
- 与 SECURITY-008（用户名碰撞）组合时，影响更严重

## 修复建议

1. **从组中移除用户**：
   ```cpp
   utils::exec_safe({"gpasswd", "-d", username, username});
   ```
2. **清除 SGID 位**：
   ```cpp
   utils::exec_safe({"chmod", "g-s", path.string()});
   ```
3. **删除 AI 用户组**：
   ```cpp
   utils::exec_safe({"groupdel", username});
   ```
4. **保存并恢复原始权限模式**：在 `grant_write_access` 时保存原始 mode，revoke 时恢复

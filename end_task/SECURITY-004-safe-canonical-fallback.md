---
source: SECURITY-004-safe-canonical-fallback.md
status: raw
createdAt: 2026-04-14T05:35:36.887Z
---
# SECURITY-004: safe_canonical() 回退到未验证路径 (CRITICAL)

# SECURITY-004: safe_canonical() 回退到未验证路径 (CRITICAL)

## 严重级别
**CRITICAL - 路径验证绕过**

## CVSS 评分
8.1 (AV:L/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:N)

## 发现位置
- `src/security/path_validator.cpp:6-13` - `safe_canonical()` 函数

## 漏洞描述

`safe_canonical()` 在 `fs::canonical()` 失败时（路径不存在、权限不足、悬挂符号链接），静默返回原始未验证路径：

```cpp
fs::path safe_canonical(const fs::path& p) {
    std::error_code ec;
    auto canonical = fs::canonical(p, ec);
    if (ec) {
        return p;  // 返回未解析的路径！
    }
    return canonical;
}
```

### 影响范围

此函数是所有路径安全验证的基础，被以下关键函数调用：
- `is_subpath()` — 挂载验证、卸载、列表
- `validate_mount_paths()` — 所有挂载路径验证
- `validate_no_circular_mount()` — 循环挂载检测
- `Graft::is_mounted()` — 挂载存在检查

### 攻击场景

1. **符号链接攻击**: 创建指向允许范围外的符号链接。如果目标不存在，`canonical()` 失败，返回未解析路径，后续检查基于未解析路径执行。

2. **路径遍历**: 路径包含 `../` 组件但不存在于磁盘时，`safe_canonical` 返回包含遍历组件的原始路径。所有安全检查被绕过。

3. **TOCTOU**: 验证通过原始路径，然后攻击者在操作执行前替换符号链接。

```bash
# 创建悬挂符号链接
ln -s /etc/shadow /home/user/project/../../../etc_passwd
# safe_canonical 返回原始路径，is_subpath("/home/user", ...) 通过
# 实际操作指向 /etc/shadow
```

## 修复建议

1. **失败时返回空路径或抛出异常**：
```cpp
fs::path safe_canonical(const fs::path& p) {
    std::error_code ec;
    auto canonical = fs::canonical(p, ec);
    if (ec) return {};  // 或 throw
    return canonical;
}
```

2. **或使用 `weakly_canonical()`**：解析存在的部分，规范化其余部分

3. **至少拒绝包含 `..` 的路径**

## 影响
- 完全绕过路径验证系统
- 挂载操作可指向任意文件系统位置
- 影响所有依赖 `safe_canonical` 的安全检查


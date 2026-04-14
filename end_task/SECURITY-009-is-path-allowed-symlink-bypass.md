---
id: SECURITY-009
severity: HIGH
cvss: 7.1
status: new
createdAt: 2026-04-14
component: utils/shell, security/path_validator
---

# SECURITY-009: is_path_allowed() 和 safe_canonical() 仍可被符号链接绕过

## 严重级别
**HIGH - 路径验证绕过**

## CVSS 评分
7.1 (AV:L/AC:H/PR:L/UI:N/S:U/C:H/I:H/A:H)

## 发现位置
- `src/utils/shell.cpp:228-254` (is_path_allowed)
- `src/security/path_validator.cpp:11-26` (safe_canonical)

## 漏洞描述

### 1. is_path_allowed() canonical 失败时回退到原始路径

```cpp
// shell.cpp:232-233
std::error_code ec;
fs::path canon = fs::canonical(p, ec);
if (ec) canon = p;  // canonical 失败 → 使用未验证的原始路径
```

`fs::canonical()` 要求路径存在。对于不存在的路径（或悬空符号链接），回退到用户提供的原始路径 `p`。攻击者可以：

1. 创建指向不存在路径的符号链接 → canonical 失败 → 使用原始路径
2. 原始路径通过 `/home/` 前缀检查 → 验证通过
3. 之后创建符号链接指向 `/etc` → 实际操作目标是 `/etc`

### 2. safe_canonical() 双重失败回退

```cpp
// path_validator.cpp:14-17
if (ec) {
    auto weak = fs::weakly_canonical(p, ec);
    if (ec) {
        return p;  // weakly_canonical 也失败 → 返回未验证路径
    }
```

虽然现在使用了 `weakly_canonical`（改进），但如果两者都失败，仍返回原始路径。更重要的是，`weakly_canonical` 对不存在路径的中间组件不做解析，攻击者可以通过 `/home/maxx/../../../etc` 构造绕过。

### 3. TOCTOU 竞态

路径验证（canonical）和实际使用（mount/useradd）之间存在时间窗口：
1. 验证时：`/home/maxx/project` → canonical → `/home/maxx/project` (合法)
2. 攻击者替换：`mv project project.bak && ln -s /etc project`
3. 实际使用：操作 `/etc`

## 攻击场景

```bash
# 场景 1: 利用 canonical 失败
ln -s /etc/passwd /home/maxx/attack
am create /home/maxx/attack  # canonical 对存在的符号链接会解析到 /etc/passwd
# 但 validate_path_allowed 会检查 /etc → 被拒

# 场景 2: 利用 TOCTOU
# 终端1: 循环替换 symlink
while true; do
    ln -sf /home/maxx/real_proj /home/maxx/link
    ln -sf /etc /home/maxx/link
done
# 终端2:
am create /home/maxx/link  # 验证时指向 real_proj，mount 时指向 /etc
```

## 影响
- 绕过所有基于路径的安全检查
- mount 路径验证、用户创建路径限制均可被绕过
- 可能导致系统目录被挂载到 AI 用户环境

## 修复建议

1. **canonical 失败时拒绝路径，不要回退**：
   ```cpp
   fs::path canon = fs::canonical(p, ec);
   if (ec) return false;  // 拒绝无法解析的路径
   ```
2. **在 safe_canonical 中添加 ".." 检查**（已有部分实现，扩展到所有路径组件）
3. **使用 O_PATH + fstat 而非路径字符串验证**：打开文件描述符后操作 fd
4. **在关键操作前后二次验证 canonical 路径一致性**

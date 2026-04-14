---
id: SECURITY-012
severity: CRITICAL
cvss: 9.1
status: reopen
createdAt: 2026-04-14
component: install
related: SECURITY-001
---

# SECURITY-012: Sudoers 通配符规则仍然存在（SECURITY-001 未修复）

## 严重级别
**CRITICAL - 权限提升**

## CVSS 评分
9.1 (AV:L/AC:L/PR:L/UI:N/S:C/C:H/I:H/A:H)

## 发现位置
- `install.sh:247-255`

## 漏洞描述

SECURITY-001 报告的 sudoers 通配符问题在最新代码中仍然完全存在，`*` 通配符允许任意参数：

```bash
# install.sh:247-255
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} create *
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mkdir * *
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} cd *
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} rm *
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} force-destroy *
```

虽然 C++ 层已添加了路径验证（`validate_path_allowed`、`is_path_allowed`），但 sudoers 层面的通配符仍然是第一道防线缺失。如果 C++ 层的验证存在任何绕过（如 SECURITY-009 的 symlink 攻击），sudoers 的 `*` 通配符不提供任何保护。

### 特别危险的命令

1. **`force-destroy *`**：sudoers 允许任意参数，C++ 层的 prefix 检查依赖 `get_effective_username()`（见 SECURITY-002）
2. **`mkdir * *`**：允许为任意路径授予任意用户写权限
3. **`create *`**：允许为任意路径创建 AI 用户

### 与旧版的对比

| 方面 | 旧版 | 新版 |
|---|---|---|
| C++ 路径验证 | 无 | 有 (`validate_path_allowed`) |
| C++ mount source 验证 | 无 | 有 (`validate_mount_source`) |
| Sudoers 通配符 | `""` + `/*` | `*` |
| 安全性提升 | — | C++ 层有改善，但 sudoers 层仍有通配符 |

## 修复建议

```bash
# 去除通配符，让 C++ 层通过 argv 自行解析
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} create
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mkdir
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} rm
# force-destroy 不应暴露给非特权用户
# cd, health, list, config, status 不需要 root
```

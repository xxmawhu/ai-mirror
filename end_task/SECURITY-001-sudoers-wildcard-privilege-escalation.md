---
source: SECURITY-001-sudoers-wildcard-privilege-escalation.md
status: raw
createdAt: 2026-04-14T05:34:08.123Z
---
# SECURITY-001: Sudoers 通配符规则允许任意路径操作 (CRITICAL)

# SECURITY-001: Sudoers 通配符规则允许任意路径操作 (CRITICAL)

## 严重级别
**CRITICAL - 权限提升**

## CVSS 评分
9.8 (AV:L/AC:L/PR:L/UI:N/S:C/C:H/I:H/A:H)

## 发现位置
- `install.sh:248-256` - sudoers 规则生成

## 漏洞描述

install.sh 生成的 sudoers 规则使用了通配符匹配，且参数之间无强关联约束：

```
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin create "",/*
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin mkdir "",/* ""
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin force-destroy "",*
```

### 关键攻击向量

1. **`create` 命令 - 可为系统关键目录创建 ai-user**:
   - `am create /etc` → 以 root 权限运行，用户名推导为 `imaxx_etc`
   - `am create /root` → 可以在 root 的 home 目录创建 ai-user 并获得写权限
   - 攻击者可以为任意系统目录创建 ai-user，并自动获得该目录的 group write 权限

2. **`mkdir` 命令 - 任意目录 + 任意用户名**:
   - `am mkdir /etc attacker_controlled_user` → 可将任意目录的 group 权限赋予任意用户
   - 路径和用户名参数完全独立，无交叉验证

3. **`force-destroy` 命令 - 可删除非 ai-mirror 管理的用户**:
   - 虽然 C++ 代码有 prefix 检查 (`user_manager.cpp:106-109`)，但 sudoers 层面的通配符 `""*` 不做此检查
   - 如果 C++ 代码被绕过或存在 TOCTOU 竞态，系统用户可被删除

### 额外问题
`""` 参数匹配空字符串，但实际调用只有 2 个参数（如 `ai-mirror-bin create /path`），sudoers 规则期望 3 个参数。这可能导致非 root 用户无法正常使用工具。

## 攻击场景

```bash
# ai-mirror 组的普通用户可以执行:
am create /etc                    # 在 /etc 创建 ai-user，获得写权限
am mkdir /root/.ssh attackername  # 授予攻击者对 root .ssh 的写权限
am force-destroy root             # 尝试删除 root 用户
```

## 修复建议

1. **sudoers 规则不使用通配符，在 C++ 层严格验证参数**:
   ```
   %ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin create
   %ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin mkdir
   %ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin list
   ```
2. **C++ 代码增加路径白名单**: `create` 命令只允许 `/home/<main_user>/` 下的路径
3. **`force-destroy` 不暴露给普通用户的 sudoers 规则**

## 影响
- ai-mirror 组成员可提权到 root 级别的文件系统写权限
- 可修改 `/etc/passwd`, `/etc/shadow`（通过 group write + SGID）
- 可植入 SSH 后门


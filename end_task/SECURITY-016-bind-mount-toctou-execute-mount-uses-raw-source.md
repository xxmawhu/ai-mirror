---
id: SECURITY-016
severity: MEDIUM
cvss: 5.5
status: new
category: race-condition
component: bind_mount TOCTOU
file: src/core/graft.cpp:90-118
discovered_at: 2026-04-14
---

# SECURITY-016: bind_mount TOCTOU 窗口：canonical 检查与 validate_path_exists 使用不同路径

## 摘要

`bind_mount()` 中的 TOCTOU 防护在两次 `safe_canonical()` 调用之间调用了 `validate_path_exists(source)`，但 `validate_path_exists()` 使用 `O_PATH | O_NOFOLLOW` 打开原始 `source`（非 canonical 路径）。如果 `source` 是符号链接，`O_NOFOLLOW` 会导致打开失败，返回 false。这意味着如果 source 是指向系统目录的符号链接，`bind_mount` 会被错误地拒绝（false negative），但如果 source 在两次 canonical 之间被替换，攻击者仍可绕过。

## 详细分析

### 漏洞代码

`src/core/graft.cpp:90-118`:
```cpp
bool Graft::bind_mount(const fs::path& source, const fs::path& target, bool read_only) {
    if (!security::validate_mount_source(source)) { return false; }

    auto pre_mount_source = security::safe_canonical(source);  // (1) canonical
    if (pre_mount_source.empty()) { return false; }

    if (is_mounted(target)) { return true; }

    if (!security::validate_path_exists(source)) { return false; }  // (2) O_NOFOLLOW

    auto pre_exec_source = security::safe_canonical(source);  // (3) canonical again
    if (pre_exec_source != pre_mount_source) { return false; }  // TOCTOU check

    return execute_mount(source, target, read_only);  // (4) uses original source
}
```

### 问题分析

1. **`execute_mount` 使用原始 `source` 而非 canonical 路径**: 第 118 行传入 `source`（用户输入）而非 `pre_exec_source`（验证后的 canonical 路径）。在 TOCTOU 窗口内（步骤 3 和步骤 4 之间），`source` 可以被替换为指向系统目录的符号链接。

2. **`validate_path_exists` 对符号链接拒绝**: 使用 `O_NOFOLLOW`，如果 source 本身是符号链接，会返回 false。这防止了符号链接攻击，但也意味着合法的符号链接源被拒绝。

3. **真正的问题**: 步骤 (3) canonical 检查通过后，在 `execute_mount` 之前存在一个微小的竞态窗口。更重要的是，`execute_mount` 传入的 `source` 是用户输入而非 canonical 路径。

### 攻击场景（理论）

1. 创建 `/home/user/mydir` 目录
2. 运行 `am create /home/user/mydir`（触发 mount 路径）
3. `safe_canonical` 解析为 `/home/user/mydir`（通过）
4. `validate_path_exists` 检查通过（是目录）
5. 第二次 `safe_canonical` 检查通过
6. **竞态窗口**: 攻击者将 `/home/user/mydir` 替换为指向 `/etc` 的符号链接
7. `execute_mount` 执行 `mount --bind /home/user/mydir target`
8. 实际 mount 了 `/etc` 的内容

虽然窗口极小，但在多核系统上可通过 `inotify` 监控来精确触发。

## CVSS 评分: 5.5 (MEDIUM)

- **AV:L/AC:H/PR:L/UI:N/S:U/C:L/I:H/A:N**
- 本地攻击，高复杂度（需要精确的竞态时序）

## 修复建议

将 `pre_exec_source`（canonical 路径）传递给 `execute_mount`，而非原始 `source`：

```cpp
return execute_mount(pre_exec_source, target, read_only);
```

或者使用 `openat()` + `fstat()` 获取 fd，通过 `/proc/self/fd/N` 传递给 mount，彻底消除竞态。

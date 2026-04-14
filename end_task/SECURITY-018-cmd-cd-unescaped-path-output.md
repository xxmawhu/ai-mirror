---
id: SECURITY-018
severity: MEDIUM
cvss: 5.0
status: new
category: access-control
component: cmd_cd
file: src/cli/commands.cpp:127-162
discovered_at: 2026-04-14
---

# SECURITY-018: cmd_cd 输出未转义的路径到 stdout，可被 wrapper 脚本 eval 执行

## 摘要

`cmd_cd()` 在多个分支直接输出未转义的路径到 stdout（第 148、160 行）。如果 wrapper 脚本或 shell 函数使用 `eval $(am cd path)` 模式来执行输出，包含空格或特殊字符的路径会被 shell 错误解析。

## 详细分析

### 漏洞代码

`src/cli/commands.cpp:147-161`:
```cpp
if (owner.empty() || owner == current_user) {
    std::cout << "cd " << target.string() << std::endl;  // 未转义
    return 0;
}
// ...
std::cout << "cd " << target.string() << std::endl;  // 未转义
return 0;
```

第 154-156 行的 SSH 分支使用了 `shell_escape()`，但第 148 和 160 行的本地 `cd` 分支没有。

### 攻击场景

1. 创建名为 `/home/user/my project` 的目录（含空格）
2. 运行 `am cd "/home/user/my project"`
3. stdout 输出: `cd /home/user/my project`
4. 如果 wrapper 使用 `eval $(am cd ...)`:
   ```bash
   eval "cd /home/user/my project"
   ```
   这会被 shell 解析为 `cd /home/user/my` 然后执行 `project` 命令。

5. 更严重的场景 — 路径含分号: 虽然 `validate_path_no_shell_metachars()` 阻止了 `;` 在 SSH 分支中出现，但本地 `cd` 分支在 metachar 检查之前就已经被使用了（metachar 检查在第 140 行，但只影响 SSH 分支逻辑的进入）。

等等，重新审查：`validate_path_no_shell_metachars()` 在第 140 行对 `target.string()` 进行检查，这会影响整个函数。但 `\` 字符不在 metachar 列表中（列表是 `;\`$(){}[]|&<>!\n\r`——实际上反引号 `` ` `` 被检查了），空格也不在列表中。

所以含空格的路径会通过验证但输出未转义。

## CVSS 评分: 5.0 (MEDIUM)

- **AV:L/AC:L/PR:L/UI:N/S:U/C:N/I:L/A:L**

## 修复建议

对所有 stdout 输出使用 `shell_escape()`：

```cpp
if (owner.empty() || owner == current_user) {
    std::cout << "cd " << utils::shell_escape(target.string()) << std::endl;
    return 0;
}
// ...
std::cout << "cd " << utils::shell_escape(target.string()) << std::endl;
return 0;
```

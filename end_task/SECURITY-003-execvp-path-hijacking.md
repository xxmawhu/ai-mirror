---
source: SECURITY-003-execvp-path-hijacking.md
status: raw
createdAt: 2026-04-14T05:35:36.870Z
---
# SECURITY-003: execvp() PATH 劫持导致 Root RCE (CRITICAL)

# SECURITY-003: execvp() PATH 劫持导致 Root RCE (CRITICAL)

## 严重级别
**CRITICAL - 远程代码执行**

## CVSS 评分
7.8 (AV:L/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H)

## 发现位置
- `src/utils/shell.cpp:58` - `do_fork_exec()` 中的 `execvp()` 调用

## 漏洞描述

`exec_safe()` 使用 `execvp()` 执行系统命令，`execvp()` 会搜索 `PATH` 环境变量。子进程继承了父进程的完整环境，没有做任何 PATH 清理：

```cpp
// shell.cpp:58
::execvp(file.c_str(), argv);
```

通过 `exec_safe()` 执行的所有 root 级别命令都受影响：`mount`, `umount`, `findmnt`, `chmod`, `chgrp`, `usermod`, `groupadd`, `getent`, `ssh-keygen`, `useradd`, `userdel`, `find`, `cp`, `chown`。

### 攻击场景

如果攻击者能操纵 `PATH` 环境变量（通过 systemd 环境、cron 任务、或直接以 root 执行），可以将 `mount` 替换为恶意二进制文件：

```bash
# 攻击者在可控目录放置恶意程序
cp /path/to/backdoor /tmp/mount
PATH=/tmp:$PATH ai-mirror-bin create /home/user/project
# 以 root 身份执行了 /tmp/mount 而非 /usr/bin/mount
```

虽然 sudo 通过 `secure_path` 缓解了此问题，但 systemd timer 和直接 root 执行不受保护。

## 修复建议

1. **使用 `execv()` 替代 `execvp()`**，使用绝对路径：
```cpp
std::string full_path = "/usr/bin/" + file;
if (!fs::exists(full_path)) full_path = "/usr/sbin/" + file;
::execv(full_path.c_str(), argv);
```

2. **或在子进程中设置安全 PATH**：
```cpp
::setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
::execvp(file.c_str(), argv);
```

3. **在程序启动时立即设置安全 PATH**，避免全局受影响

## 影响
- 通过 PATH 劫持实现 root 级别任意代码执行
- 所有通过 exec_safe 执行的命令均受影响
- sudo 之外的执行路径（systemd timer、cron）尤其危险


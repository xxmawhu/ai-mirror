# 03 - 安全模型与权限设计

## 概述

ai-mirror 采用多层安全模型，从路径验证到文件操作全链路防护。

## 安全层级

```
┌──────────────────────────────────────┐
│  Layer 1: Sudoers 规则                │  外部门控
│  - 无通配符，精确子命令授权            │
│  - 仅 ai-mirror 组成员可执行          │
├──────────────────────────────────────┤
│  Layer 2: AI 用户隔离检测             │  身份验证
│  - is_ai_user() 拒绝 AI 用户调用      │
│  - is_group_member() 组成员检查       │
├──────────────────────────────────────┤
│  Layer 3: 路径验证                    │  边界防护
│  - SYSTEM_DIRS 黑名单 (17 个)         │
│  - fs::canonical() 解析               │
│  - ".." 拒绝                          │
│  - allowed_bases 白名单               │
├──────────────────────────────────────┤
│  Layer 4: 文件操作安全                │  TOCTOU 防护
│  - O_NOFOLLOW 全链路                  │
│  - fd-based 操作 (fchown/fchmod)      │
│  - O_EXCL 创建                        │
│  - /dev/urandom 随机后缀              │
├──────────────────────────────────────┤
│  Layer 5: Shell 工具安全              │  命令执行
│  - 命令白名单                          │
│  - 硬编码绝对路径                      │
│  - fork()+execv() 替代 popen()        │
└──────────────────────────────────────┘
```

## 路径验证

### SYSTEM_DIRS 黑名单

```cpp
static const std::vector<std::string> SYSTEM_DIRS = {
    "/etc", "/root", "/var", "/proc", "/sys", "/dev",
    "/boot", "/lib", "/usr", "/sbin", "/bin", "/run",
    "/opt", "/tmp", "/srv", "/media", "/lost+found"
};
// 故意排除 /mnt（BeeGFS、NFS 挂载点需要支持）
```

### 路径验证函数

| 函数 | 文件 | 用途 |
|------|------|------|
| `validate_path_allowed()` | path_validator.cpp | SYSTEM_DIRS 黑名单检查 |
| `is_path_allowed()` | shell.cpp | 完整权限检查：归属 + 写权限 + allowed_bases |
| `validate_mount_source()` | path_validator.cpp | mount 源验证 |
| `validate_mount_paths()` | path_validator.cpp | mount 路径对验证（循环检测） |
| `is_subpath()` | path_validator.cpp | 父子路径关系检查 |
| `safe_canonical()` | path_validator.cpp | 安全 fs::canonical()，空路径=拒绝 |

### `is_path_allowed()` 检查流程

```
输入: path, main_user, allowed_bases
  │
  ├── 拒绝 ".." 组件
  ├── 获取 login_uid（/proc/self/loginuid，忽略环境变量）
  ├── 已存在路径: fs::canonical()
  ├── 不存在路径: 验证父目录存在且 canonical
  ├── 检查归属:
  │   ├── owner uid == login_uid
  │   ├── 或路径在 AI 用户 home 下
  │   ├── 或路径在 allowed_bases 下
  │   └── 最多遍历 32 层父目录
  └── 检查写权限
```

## TOCTOU 防护

### fd-based 文件操作

所有 chown/chmod 操作通过文件描述符执行：

```cpp
// 打开时不跟随符号链接
int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
// 通过 fd 操作（不受路径替换影响）
fchown(fd, uid, gid);
fchmod(fd, mode);
```

### 递归 chown (chown_recursive_fd)

```cpp
// 使用 openat() 相对于父目录 fd 打开，防止遍历中的符号链接注入
int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
if (errno == ELOOP) {
    // 符号链接：使用 fchownat(AT_SYMLINK_NOFOLLOW)
    fchownat(dirfd, name, uid, gid, AT_SYMLINK_NOFOLLOW);
}
```

- 最大深度：1000 层
- 每个组件 O_NOFOLLOW 检测符号链接

### 安全目录创建 (safe_create_directories)

```cpp
// 逐组件创建，每步使用 O_NOFOLLOW
mkdirat(parent_fd, component, mode);
// 创建后检测符号链接替换（TOCTOU 检测）
```

## 命令执行安全

### exec_safe()

```cpp
ShellResult exec_safe(const std::vector<std::string>& args);
```

**安全措施**：
1. 命令白名单（22 个命令）
2. 硬编码绝对路径解析（无 PATH 查找）
3. `fork() + execv()` 替代 `popen()`/`system()`

### 命令白名单

```
mount, umount, chmod, chown, chgrp,
useradd, userdel, groupadd, groupdel, usermod, passwd,
gpasswd, ssh-keygen, mkdir, cp, mv,
getent, findmnt, which, ssh, pkill, ps
```

### 硬coded 路径映射

```cpp
{"mount", "/usr/bin/mount"},
{"umount", "/usr/bin/umount"},
{"chmod", "/usr/bin/chmod"},
{"chown", "/usr/bin/chown"},
{"useradd", "/usr/sbin/useradd"},
{"userdel", "/usr/sbin/userdel"},
// ... 全部 22 个命令
```

## SSH 安全

### 密钥管理

- 默认密钥类型：ed25519
- RSA 密钥：强制 `-b 4096`
- ECDSA 密钥：强制 `-b 521`
- 私钥权限检查：警告非 0600

### authorized_keys 写入

```
1. 生成 64-bit 随机后缀 (/dev/urandom)
2. O_CREAT|O_EXCL|O_NOFOLLOW 创建临时文件（3 次重试）
3. 写入内容
4. fchown(fd, ai_user_uid, ai_user_gid)
5. fchmod(fd, 0600)
6. rename() 原子替换
```

### SSH 公钥验证

```cpp
bool validate_ssh_public_key(const std::string& key);
```

- 最大长度：8192 字符
- 拒绝引号、换行符
- 必须以已知密钥类型前缀开头：
  - `ssh-ed25519`, `ssh-rsa`
  - `ecdsa-sha2-nistp256`, `ecdsa-sha2-nistp384`, `ecdsa-sha2-nistp521`
  - `sk-ssh-ed25519@openssh.com`, `sk-ecdsa-sha2-nistp256@openssh.com`
- 拒绝带 SSH options 前缀的密钥（如 `command="/bin/false" ssh-ed25519 ...`）
- Base64 载荷字符验证：`[a-zA-Z0-9+/=@._-]`

### StrictModes 兼容

sshd `StrictModes=yes` 要求：
- home_dir：无 g+w（移除）
- `~/.ssh/`：700，owner = AI 用户
- `~/.ssh/authorized_keys`：600，owner = AI 用户

## 用户名验证

```cpp
bool validate_username(const std::string& username);
```

- 格式：`{prefix}_{main_user}_{path_hash6}`
- `path_hash6`: 6 位 hex 字符（`[a-f0-9]`）
- 仅 `[a-z0-9_]`（无连字符）
- 最大 32 字符
- 首字符不能为数字

### 前缀碰撞防护

```cpp
bool validate_ai_user_ownership(const std::string& ai_user,
                                const std::string& main_user,
                                const std::string& prefix);
```

- 格式：`prefix + main_user + "_"`
- 确保 `_` 后至少还有一个字符
- 防止 `alice` 匹配 `alice2`

## 状态文件安全

### .am_status 文件

- 位置：AI 用户 home_dir 根目录
- 权限：root:root 所有（AI 用户无法篡改）
- PoW 验证：`md5(content).substr(0,3) == "000"`（12-bit 难度，~4096 次尝试）
- 使用微秒时间戳作为 nonce

### 配置文件安全

```cpp
validate_config_file_security();
```

- 检查文件 owner == login_uid
- 允许符号链接（但验证目标文件归属）
- 警告 group/world 可写
- 最大文件大小：1MB

## Shell Wrapper 安全

`profile/am.sh` 在 shell 层额外验证：

1. **路径验证**：`_am_validate_path()` 检查路径在 HOME、/home、BeeGFS、/scratch 下
2. **AI 用户验证**：`_am_validate_ai_user()` 检查用户名格式 `i{main_user}_*`
3. **输出解析**：验证 action、user、path 非空
4. **SSH 密钥**：检查密钥文件存在

## mv/cp 所有权模型

### 双向 ai-user 检测

`cmd_mv` 和 `cmd_cp` 使用双向检测策略确定文件归属：

```
源路径检测:
  1. detect_ai_user_from_path(src_path)   # 路径组件检测
  2. detect_ai_user_from_path(parent)     # 父目录路径检测
  3. detect_owner_user(src_path)          # stat uid → username

目标路径检测:
  1. detect_ai_user_from_path(dst_path)   # 路径组件检测
  2. detect_owner_user(dst_path)          # stat 目录 owner（dst 为目录时）
```

### 5 种场景矩阵

| 场景 | src 归属 | dst 归属 | 行为 |
|------|----------|----------|------|
| A. main→ai | main-user | ai-user-A | chown 到 ai-user-A |
| B. ai→ai(同main) | ai-user-A | ai-user-B | chown 到 ai-user-B |
| C. ai→main | ai-user-A | main-user | chown 回 main-user |
| D. 跨 main-user | ai-user-A(main1) | ai-user-B(main2) | **拒绝** |
| E. main→main | main-user | main-user | 普通 mv，无 chown |

### 安全校验流程

```
1. src ai-user 安全检查:
   validate_ai_user_ownership(src_ai_user, main_user, prefix)
   → 失败: "Source belongs to ai-user 'xxx' which does not belong to you"

2. dst ai-user 安全检查:
   validate_ai_user_ownership(dst_ai_user, main_user, prefix)
   → 失败: "Destination ai-user 'xxx' does not belong to user 'yyy'"

3. 确定目标 owner:
   dst 有 ai-user → chown_user = dst_ai_user
   src 有 ai-user 且 dst 无 → chown_user = main_user (回收到主用户)
   两边都无 → 不 chown
```

### stat fallback 机制

当路径组件中无法检测到 ai-user（如 `~/sim_match/` 不包含 ai-user 名），使用 `detect_owner_user()` 通过 `stat()` 获取目录的 uid，再通过 `getpwuid()` 解析为用户名。此方法适用于：

- 目标目录由 `am mkdir` 创建（owner 为 ai-user）
- 目标目录在 ai-user home 下但路径不含 ai-user 名组件

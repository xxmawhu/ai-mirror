---
source: SECURITY-006-arbitrary-mount-via-config.md
status: raw
createdAt: 2026-04-14T05:36:06.987Z
---
# SECURITY-006: 用户可控配置允许绑定挂载任意路径 (CRITICAL)

# SECURITY-006: 用户可控配置允许绑定挂载任意路径 (CRITICAL)

## 严重级别
**CRITICAL - 信息泄露 / 权限提升**

## CVSS 评分
8.8 (AV:L/AC:L/PR:L/UI:N/S:C/C:H/I:H/A:H)

## 发现位置
- `src/cli/commands.cpp:71-80` - 挂载路径直接使用
- `src/core/config.cpp:109-122` - 从用户主目录加载配置

## 漏洞描述

`cmd_create()` 直接使用配置文件中的 `mount.paths` 进行绑定挂载，无路径白名单验证。配置文件位于用户主目录 `~/.ai-mirror.toml`，用户可完全控制：

```cpp
// commands.cpp:71-80
for (const auto& mount_path : ctx.config.mount.paths) {
    fs::path source = core::PathResolver::resolve(mount_path.string());
    if (!fs::exists(source)) continue;
    fs::path target = core::PathResolver::to_ai_user_path(source, user_info.username, main_user);
    ctx.graft->bind_mount(source, target, true);  // 以 root 执行 bind mount
}
```

### 攻击场景

ai-mirror 组成员编辑 `~/.ai-mirror.toml`：

```toml
[mount]
paths = ["/etc", "/root", "/var/lib/docker"]
```

运行 `am create /some/project` 时，二进制以 root（通过 sudo）将敏感目录以只读方式绑定挂载到 AI 用户的 home。然后通过 SSH 以 AI 用户身份读取：

- `/etc/shadow` → 离线密码破解
- `/root/.ssh/` → 横向移动
- `/var/lib/docker` → 容器逃逸凭据

### 配置文件无权限检查

`ConfigParser::load()` (`config.cpp:36`) 不验证配置文件的所有权或权限。攻击者如果能写入 `~/.ai-mirror.toml`，就能注入任意配置。

## 修复建议

1. **验证 mount.paths 白名单**: 只允许用户主目录下的路径
2. **拒绝敏感路径**: `/etc`, `/root`, `/var`, `/proc`, `/sys`, `/boot`
3. **配置文件存储在 `/etc/ai-mirror/`**: root 所有，mode 0600
4. **加载前验证配置文件权限**: 检查 owner 和 mode
5. **validate_mount_paths() 增加源路径白名单检查**

## 影响
- 任何 ai-mirror 组成员可读取系统上任意 root 可读文件
- 读取 `/etc/shadow` → 密码破解 → 完全提权
- 读取 Docker 凭据 → 容器逃逸


---
source: SECURITY-005-force-cleanup-no-validation.md
status: raw
createdAt: 2026-04-14T05:36:06.979Z
---
# SECURITY-005: force_cleanup() 无验证卸载任意挂载点 (CRITICAL)

# SECURITY-005: force_cleanup() 无验证卸载任意挂载点 (CRITICAL)

## 严重级别
**CRITICAL - 拒绝服务 / 文件系统破坏**

## CVSS 评分
8.4 (AV:L/AC:L/PR:L/UI:N/S:U/C:N/I:H/A:H)

## 发现位置
- `src/daemon/mount_cleaner.cpp:35-47` - `MountCleaner::force_cleanup()`

## 漏洞描述

`force_cleanup()` 接受任意路径列表并直接卸载，没有任何验证：

```cpp
int MountCleaner::force_cleanup(const std::vector<fs::path>& mounts) {
    int cleaned = 0;
    for (const auto& m : mounts) {
        auto result = utils::exec_safe({"umount", "-l", m.string()});
        // 无验证 m 是否在允许范围内
```

### 攻击场景

如果攻击者能控制或影响输入（通过被篡改的 `find_stale_mounts()` 或任何调用者），可以卸载任意已挂载文件系统：

```bash
# 卸载根文件系统
force_cleanup({"/"})

# 卸载其他用户的工作区
force_cleanup({"/home/otheruser/projects/data"})

# 卸载关键系统分区
force_cleanup({"/boot", "/var", "/home"})
```

`-l`（lazy）标志意味着卸载立即对新访问生效，即使有进程正在使用。

## 修复建议

在卸载前验证每个路径：
```cpp
for (const auto& m : mounts) {
    auto canon = security::safe_canonical(m);
    if (canon.empty() || canon.string().find("/home/") != 0) {
        logger->error("Refusing to unmount path outside /home: {}", m.string());
        continue;
    }
    // 验证属于预期的 ai-mirror 用户
    // ... 卸载
}
```

## 影响
- 可卸载任意文件系统，包括 `/`, `/home`, `/boot`, `/var`
- 导致系统范围的拒绝服务
- 多用户环境下可卸载其他用户的工作区


---
id: SECURITY-017
severity: MEDIUM
cvss: 5.3
status: new
category: information-disclosure
component: authorize_key
file: src/core/ssh_manager.cpp:71-108
discovered_at: 2026-04-14
---

# SECURITY-017: authorized_keys 追加写入无原子性，可导致 SSH 密钥注入

## 摘要

`authorize_key()` 和 `authorize_public_key_string()` 使用非原子的"读取 → 追加"模式写入 `authorized_keys`。在并发场景下，一个用户的密钥注入操作可能被另一个操作覆盖，或恶意进程可在读取和写入之间注入自己的密钥。

## 详细分析

### 漏洞代码

`src/core/ssh_manager.cpp:110-151` (authorize_public_key_string):
```cpp
bool SSHManager::authorize_public_key_string(...) {
    // ...
    {
        std::ifstream ifs(auth_keys);      // (1) 读取现有内容
        bool already_exists = false;
        // ... 检查是否已存在 ...
        if (!already_exists) {
            std::ofstream ofs(auth_keys, std::ios::app);  // (2) 追加写入
            ofs << public_key << "\n";
        }
    }
    // (3) chown + chmod
}
```

`src/core/ssh_manager.cpp:71-108` (authorize_key):
```cpp
bool SSHManager::authorize_key(...) {
    // ...
    {
        std::ifstream ifs(public_key_path);  // 读取公钥文件
        std::string key_content(...);
        std::ofstream ofs(auth_keys, std::ios::app);  // 直接追加，无去重
        ofs << key_content << "\n";
    }
}
```

### 攻击向量

1. **竞态注入**: 攻击者在步骤 (1) 和步骤 (2) 之间，通过另一个进程向 `authorized_keys` 追加自己的密钥
2. **重复条目**: `authorize_key()` 无去重检查，多次调用会追加重复的密钥条目
3. **TOCTOU on file path**: `auth_keys` 路径在 `ensure_ssh_dir` 和实际写入之间可能被替换为符号链接

### 影响

如果攻击者在 `authorized_keys` 中注入了自己的公钥，可以通过 SSH 以 ai-user 身份登录，绕过隔离。

## CVSS 评分: 5.3 (MEDIUM)

- **AV:L/AC:H/PR:L/UI:N/S:U/C:L/I:H/A:N**

## 修复建议

1. 使用原子写入模式（写入临时文件 → rename）：
```cpp
fs::path tmp = auth_keys.string() + ".tmp";
std::ofstream ofs(tmp);
// 写入所有密钥
ofs.close();
fs::rename(tmp, auth_keys);  // atomic on same filesystem
```

2. 在写入前使用 `flock()` 或 `O_EXCL` 锁定文件

3. 对 `authorize_key()` 也添加去重检查（与 `authorize_public_key_string` 一致）

# grant_write_access 与 sshd StrictModes 冲突

## 严重性
High

## 场景
`am create` 调用 `grant_write_access()` 将 AI 用户 home 目录设为 775（group writable），
但 sshd `StrictModes yes` 要求用户 home 目录不能 group writable，
导致 SSH 公钥认证失败：`Permission denied (publickey)`。

## 根因
`graft.cpp:526` — `new_mode = st.st_mode | (S_IRGRP | S_IWGRP | S_IXGRP)` 对 AI 用户 home 目录加了 g+w。
sshd StrictModes 检查 home 目录权限，发现 group writable 后拒绝 authorized_keys。

## 影响
- 所有通过 `am create` 创建的 AI 用户无法 SSH 登录（如果 sshd 使用默认 StrictModes yes）
- 当前测试环境使用 StrictModes no 绕过

## 修复建议
1. **方案 A**: home 目录保持 755（去掉 g+w），只对项目子目录（非 .ssh）设置 group write
2. **方案 B**: 将 authorized_keys 放到非 home 位置（如 `/var/lib/ai-mirror/<user>/authorized_keys`），通过 `AuthorizedKeysFile` 指定
3. **方案 C**: grant_write_access 后，对 `.ssh` 目录确保 700、home 确保非 group writable

## 涉及文件
- `src/core/graft.cpp` — `grant_write_access()` L526
- `src/cli/commands.cpp` — `do_configure()` 中 setgid 清理逻辑

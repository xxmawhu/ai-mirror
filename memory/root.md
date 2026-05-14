# ai-mirror 最高准则

## 1. 安全第一
- 所有用户操作必须通过 `exec_safe()` 白名单执行
- 不允许绕过白名单直接调用 system() 或 popen()
- SSH 密钥管理必须遵循最小权限原则

## 2. 子模块同步
- 每次修改必须同步到 3 个子模块（gitee / gitlib / github）
- 主仓必须更新子模块引用并推送
- 不得只推送主仓而忽略子模块

## 3. 构建验证
- 所有修改必须通过编译验证后才能提交
- commit hook 三阶段检查（代码检查 + 编译 + 单测）不可跳过
- merge hook 部署流程不可跳过

## 4. 日志规范
- error/fatal 仅用于真实故障
- 可容忍的异常降级为 warning
- 正常流程使用 info/debug

## 5. 文件操作
- 不复制/不链接/不 bind mount 敏感文件（如 known_hosts）
- 使用 ssh-keyscan 等工具独立获取信息
- 所有路径操作使用 fs::path，不硬编码

## 6. BeeGFS 兼容性
- 处理 `//deleted` 文件和 stale mount
- 使用 `umount -l`（lazy unmount）
- 递归遍历限制深度（depth=3），避免性能问题

## 7. Agent 行为约束
- 不得自动修改 memory/root.md（需用户确认）
- 不得自动执行 git push --force
- 不得自动跳过 hooks（--no-verify）
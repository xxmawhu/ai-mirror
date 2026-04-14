---
id: SECURITY-007
severity: HIGH
cvss: 7.5
status: new
createdAt: 2026-04-14
component: cli/commands
---

# SECURITY-007: cmd_cd SSH 远程命令注入

## 严重级别
**HIGH - 远程代码执行（以 AI 用户身份）**

## CVSS 评分
7.5 (AV:L/AC:H/PR:L/UI:N/S:U/C:H/I:H/A:H)

## 发现位置
- `src/cli/commands.cpp:149-151`

## 漏洞描述

`cmd_cd` 构造 SSH 命令时，将 `shell_escape(target)` 的输出嵌入到单引号包裹的远程命令字符串中。虽然 `shell_escape` 对本地 shell 是安全的（用单引号包裹），但拼接后的字符串通过 SSH 传递到远程 shell 执行，远程 shell 会将拼接结果中的 `;` 解析为命令分隔符。

```cpp
// commands.cpp:149-151
std::cout << "exec ssh -q " << utils::shell_escape(owner)
          << "@localhost -t 'cd "
          << utils::shell_escape(target.string()) << "; exec bash -l'" << std::endl;
```

### 拼接过程

对正常路径 `/home/maxx/project`：
- shell_escape → `'/home/maxx/project'`
- 输出：`exec ssh -q 'maxx'@localhost -t 'cd '/home/maxx/project'; exec bash -l'`
- Bash 拼接结果：`cd /home/maxx/project; exec bash -l` ← 安全

对恶意路径 `/home/maxx/; id; echo `：
- shell_escape → `'/home/maxx/; id; echo '`
- 输出：`exec ssh -q 'maxx'@localhost -t 'cd '/home/maxx/; id; echo ''; exec bash -l'`
- 远程 shell 执行：`cd /home/maxx/; id; echo ; exec bash -l` ← **注入 `id` 命令**

### 攻击场景

```bash
# 创建含 shell 元字符的项目路径
mkdir -p "/home/maxx/; rm -rf /home/imaxx_test; echo "
am create "/home/maxx/; rm -rf /home/imaxx_test; echo "

# 当用户或 wrapper eval cmd_cd 输出时：
am cd "/home/maxx/; rm -rf /home/imaxx_test; echo "
# SSH 远程执行: cd /home/maxx/; rm -rf /home/imaxx_test; echo ; exec bash -l
# → 以 AI 用户身份删除目标目录
```

## 影响
- 攻击者创建含特殊字符的路径即可在 AI 用户 SSH session 中注入任意命令
- 可用于删除 AI 用户文件、修改 authorized_keys、窃取数据
- 命令以 AI 用户身份执行（非 root），但可横向影响同用户的其他项目

## 修复建议

1. **不在 SSH 远程命令中拼接路径**：使用环境变量传递路径
   ```cpp
   // 修复方案
   std::cout << "exec ssh -q " << utils::shell_escape(owner)
             << "@localhost -t " << utils::shell_escape(
                 "cd " + target.string() + "; exec bash -l")
             << std::endl;
   ```
2. **验证路径不含 shell 元字符**：在 `cmd_cd` 入口处拒绝含 `;|&$(){}[]<>\`!#~` 的路径
3. **使用 `--` 分隔**：`cd -- "$PATH"` 防止路径被解释为选项

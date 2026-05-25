# 04 - 配置参考

## 配置文件

**默认路径**：`~/.ai-mirror.toml`

首次运行时自动创建（原子 O_EXCL|O_NOFOLLOW），若文件不存在则使用默认值。

## 配置结构

```toml
[user]
prefix = "i"                                          # AI 用户名前缀
allowed_bases = []                                     # 额外允许的基础路径

[mount]
paths = []                                             # 要 bind mount 的路径列表

[ssh]
key_type = "ed25519"                                   # SSH 密钥类型
key_path = ""                                          # SSH 密钥路径（空=自动检测）
ai_default_key = ""                                    # AI 用户默认公钥路径

[ai-user]
groups = ["docker"]                                    # AI 用户附加组列表
```

## 字段详解

### `[user]` 部分

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `prefix` | string | `"i"` | AI 用户名前缀。格式：`{prefix}_{main_user}_{project}` |
| `allowed_bases` | path[] | `[]` | 额外允许的基础路径（除 $HOME 外）。支持 `{user}` 占位符 |

**allowed_bases 示例**：

```toml
[user]
allowed_bases = [
    "/mnt/beegfs_data/usr/{user}",    # BeeGFS HPC 路径
    "/scratch/{user}"                  # 本地 scratch 路径
]
```

`{user}` 占位符在加载时替换为当前主用户名。

### `[mount]` 部分

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `paths` | path[] | `["~/.bashrc", "~/.config", "~/.local/bin"]` | 要 bind mount 到 AI 用户 home 的路径 |

**自动创建默认配置**：

```toml
[mount]
paths = [
    "~/.bashrc",       # Shell 配置
    "~/.config",       # 应用配置目录
    "~/.local/bin"     # 用户二进制目录
]
```

**路径解析**：
- `~/` 展开为有效 HOME 目录
- 不存在的路径跳过（不报错）
- 每个路径通过 `PathResolver::resolve()` 解析

### `[ssh]` 部分

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `key_type` | string | `"ed25519"` | SSH 密钥类型：`ed25519`、`rsa`、`ecdsa` |
| `key_path` | path | 自动检测 | AI-mirror 管理密钥路径 |
| `ai_default_key` | path | `"~/.ssh/id_ed25519.pub"` | 安装到 AI 用户的默认公钥 |

**密钥自动检测优先级**（key_path 为空时）：

```
~/.ssh/id_ed25519 > ~/.ssh/id_rsa > ~/.ssh/id_ecdsa > ~/.ssh/ai-mirror
```

**key_path**：用于主用户→AI 用户的免密 SSH，默认 `~/.ssh/ai-mirror`

**ai_default_key**：复制主用户的 SSH 公钥到 AI 用户的 `~/.ssh/`，使 AI 用户也能使用该密钥（如 git 访问）

### `[ai-user]` 部分

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `groups` | string[] | `["docker"]` | AI 用户创建时自动加入的附加组 |

**安全规则**：

1. **禁止 `ai-mirror` 组**：AI 用户绝不能加入 `ai-mirror` 组（防止权限提升）
2. **主用户必须已在组中**：防止主用户通过 AI 用户获得自己没有的组权限
3. **组不存在时跳过**：警告并继续，不阻塞创建

**默认配置**：

```toml
[ai-user]
groups = ["docker"]    # 允许 AI 用户使用 Docker（无需 sudo）
```

**自定义示例**：

```toml
[ai-user]
groups = ["docker", "render"]    # Docker + GPU 渲染组
```

**创建流程中的组添加**（`am create`）：

```
am create /path/to/project
  │
  ├── 创建 AI 用户 (useradd)
  ├── 主用户 ↔ AI 用户互加组（双向访问）
  └── 遍历 [ai-user].groups
      │   ├── 检查组存在
      │   ├── 检查主用户是否在组中（安全）
      │   └── usermod -aG <group> <ai-user>
```

## 配置加载流程

```
ConfigParser::load_default()
  │
  ├── 默认路径: ~/.ai-mirror.toml
  ├── 文件不存在?
  │   └── create_default_config() → 原子创建
  ├── validate_config_file_security()
  │   ├── 检查 owner == login_uid
  │   ├── 允许符号链接（验证目标归属）
  │   └── 警告 group/world 可写
  ├── load(path) → TOML 解析
  │   ├── 文件大小 <= 1MB
  │   ├── 解析 [user], [mount], [ssh], [ai-user]
  │   ├── 展开 {user} 占位符
  │   └── SSH key_path 自动检测
  └── 返回 Config 结构体
```

## 配置保存

`ConfigParser::save()` 使用原子写入：
1. O_EXCL|O_NOFOLLOW 创建临时文件
2. 写入内容
3. 验证符号链接目标归属
4. chown 到 login_uid
5. rename() 原子替换

## 环境变量

| 变量 | 用途 | 示例 |
|------|------|------|
| `HOME` | 有效 HOME 目录（覆盖 passwd） | `HOME=/mnt/beegfs_data/usr/maxx` |
| `AI_MIRROR_BIN` | Shell wrapper 二进制搜索路径 | `/usr/local/bin/ai-mirror-bin` |

**注意**：不使用 `SUDO_USER` / `PKEXEC_UID`（防止环境变量欺骗），使用 `/proc/self/loginuid` 确定真实用户。

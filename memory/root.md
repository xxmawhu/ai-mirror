# ai-mirror 最高准则

## 1. 安全第一
- 所有用户操作必须通过 `exec_safe()` 白名单执行
- 不允许绕过白名单直接调用 system() 或 popen()

## 2. 权限模型铁律（不可更改）

### 核心原则
- **权限模型的实现逻辑不可更改**
- 这是项目的最高安全保证，任何修改都必须经过用户明确批准
- Agent 无权自动修改权限模型相关的任何代码

### AI 用户操作铁律（最高优先级）
- **唯一操作通道**：对 AI 用户进行操作的唯一办法是主用户通过 SSH 发送执行命令
- **禁止 group write 共享**：AI 用户的 home 目录不得添加 group write permission (g+w)
- **写入权限严格分离**：每个用户（包括 AI 用户）的写入权限必须严格独立，不得通过组权限共享
- **主用户协作方式**：主用户需要操作 AI 用户目录时，必须通过 SSH 到 AI 用户执行，而非通过共享组写权限

### 设计原则
- `fix_home_dir_permissions()` 不得给 AI 用户 home 添加 g+w
- AI 用户 home 目录权限应为 0755（owner rwx，group/other r-x）
- 主用户通过 SSH 到 AI 用户执行命令来创建子项目或修改文件
- bind mount 是只读共享机制，不涉及写权限共享

### 违规处理
- 任何涉及权限模型的需求必须向用户报告
- 禁止以"优化"、"重构"名义修改权限模型
- 禁止绕过权限检查添加新的执行路径
- 禁止以"协作"为由给 AI 用户目录添加 group write

### 例外情况
仅以下情况可修改权限模型：
1. 用户通过 TG 明确确认（需要保留确认记录）
2. 修复安全漏洞（需要详细的安全审计报告）

## 3. 子模块同步
- 每次修改必须同步到 3 个子模块（gitee / gitlib / github）
- 主仓必须更新子模块引用并推送
- 不得只推送主仓而忽略子模块

## 4. 构建验证
- 所有修改必须通过编译验证后才能提交
- commit hook 三阶段检查（代码检查 + 编译 + 单测）不可跳过
- merge hook 部署流程不可跳过

## 5. 部署流程（双目录架构）

### 目录职责
| 目录 | 职责 | 用途 |
|------|------|------|
| `~/dev/aimirror/ai-mirror` | 开发目录 | 代码修改、开发、测试、agent 工作 |
| `~/release/ai-mirror` | 生产目录 | 实际执行、git pull、自动部署 |

### 自动部署机制
- **触发时机**：`~/release/ai-mirror` 执行 `git pull` 或 `git merge` 时
- **自动执行**：post-merge hook 自动调用 `install.sh`
- **部署目标**：`/usr/local/bin/am` + `/usr/local/bin/ai-mirror-bin`
- **用户操作**：用户只需执行 `git pull` + `source ~/.bashrc`，无需手动 `install.sh`

### 工作流程
```
用户在 ~/release/ai-mirror:
git pull
  ↓
post-merge hook 自动触发
  ↓
自动执行 install.sh
  ↓
部署到 /usr/local/bin
  ↓
source ~/.bashrc（刷新环境）
  ↓
am 命令可用新版本
```

### 开发目录同步
- 开发目录修改后 → commit + push
- 生产目录 `git pull` → 自动部署
- 禁止在生产目录直接修改代码

### 部署验证
- post-merge hook 必须包含部署日志
- 部署失败必须输出错误信息
- 部署成功后验证 `am --version`

### 不可变规则
- 生产目录必须有 post-merge hook
- post-merge hook 必须调用 `install.sh`
- 用户无需手动执行 `install.sh`
- 开发修改通过 git pull 自动传递到生产环境

## 6. 日志规范
- error/fatal 仅用于真实故障
- 可容忍的异常降级为 warning
- 正常流程使用 info/debug

## 6. 文件操作
- 不复制/不链接/不 bind mount 敏感文件（例外：known_hosts 可复制到 AI user 的 .ssh 目录）
- SSH known_hosts 复制仅限：同主用户创建的 AI user，复制后 chown/chmod 确保权限正确
- 其他敏感文件（如私钥、证书）禁止复制
- 所有路径操作使用 fs::path，不硬编码

## 7. BeeGFS 兼容性
- 处理 `//deleted` 文件和 stale mount
- 使用 `umount -l`（lazy unmount）
- 递归遍历限制深度（depth=3），避免性能问题

## 8. Agent 行为约束
- 不得自动修改 memory/root.md（需用户确认）
- 不得自动执行 git push --force
- 不得自动跳过 hooks（--no-verify）

## 9. 双二进制架构（am + ai-mirror-bin）

### 设计原则
- **用户透明**：用户直接调用 `am`，无需手动 sudo
- **自动提权**：wrapper 检测非 root → 自动 sudo 重执行 ai-mirror-bin
- **最小权限**：sudoers 仅允许 ai-mirror 组执行 ai-mirror-bin（无通配符）

### 文件布局
| 文件 | 用途 | 权限 |
|------|------|------|
| `/usr/local/bin/am` | wrapper，自动 sudo | 0755 |
| `/usr/local/bin/ai-mirror-bin` | 实际实现 | 0755 |
| `/etc/ai-mirror/sudoers.d/ai-mirror` | sudoers 规则 | 0440 |

### 执行流程
```
用户: am create /path
  ↓
wrapper: 检测 UID != 0
  ↓
wrapper: 检测是否在 ai-mirror 组
  ↓ (是)
wrapper: exec sudo --preserve-env=HOME ai-mirror-bin create /path
  ↓
ai-mirror-bin: 以 root 执行实际逻辑
```

### sudoers 规则示例
```
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin create ""
%ai-mirror ALL=(root) NOPASSWD: /usr/local/bin/ai-mirror-bin update ""
...（所有需要 root 的子命令）
```

### 不可变规则
- 此架构必须保持向后兼容
- 用户调用 `am` 时不应感知 sudo 的存在
- wrapper 只做 sudo 包装，不做其他逻辑
- 所有功能逻辑在 ai-mirror-bin 中实现

## 10. 子模块暴露最小原则

### 核心要求
- **最小暴露**：子模块仅包含编译和安装必需的文件，禁止推送无关内容
- **自动过滤**：同步脚本必须智能筛选，排除开发/内部文件
- **编译保证**：子模块必须能在独立环境下编译安装成功
- **禁止测试**：子模块不包含任何测试目标、测试文件或测试依赖

### 必须包含的文件
以下文件必须同步到子模块（编译/安装必需）：
- 源码：`src/**/*.cpp`, `include/**/*.hpp`
- 构建：`CMakeLists.txt`（最小版本，不含 test targets）
- 安装：`install.sh`
- 文档：`README.md`（公共仓库）
- 补齐：`completions/am-completion.bash`

### 必须排除的文件
以下文件禁止同步到子模块（开发/内部专用）：
- Agent 配置：`AGENTS.md`
- 记忆系统：`memory/**/*.md`
- 开发工具：`.pre-commit-config.yaml`, `scripts/**/*`
- 实验记录：`memory/experiments.md`
- 内部文档：`llm-docs/**/*`
- 配置模板：`*.toml`（用户自己创建）
- 测试文件：`tests/**/*`（子模块不包含任何测试）

### CMakeLists.txt 子模块规则
子模块的 `CMakeLists.txt` 必须是主仓的最小版本：
- ✅ 保留：`add_executable(am ...)`, `add_executable(ai-mirror-bin ...)`, `install(...)`
- ❌ 禁止：`enable_testing()`, `add_executable(test_...)`, `add_test(...)`, `if(EXISTS tests)` 块
- 子模块 CMakeLists.txt 行数应显著少于主仓（当前约 129 行 vs 主仓 183 行）

### 自动同步机制
- **触发时机**：每次 commit 后自动触发（通过 commit hook）
- **智能筛选**：脚本自动识别变更文件，按规则过滤
- **验证保证**：同步后验证子模块可独立编译安装
- **失败回滚**：验证失败则不更新子模块引用

### 验证流程
同步脚本必须验证：
1. 子模块 `cmake --build build-test` 成功
2. 子模块 `bash install.sh --build` 成功
3. 文件数量统计（应远小于主仓）
4. 编译产物可执行
5. CMakeLists.txt 不含 test targets（`grep -c "add_test\|enable_testing"` 应为 0）

### 违规后果
- 推送无关文件到子模块 = 违反 root.md 最高准则
- 需要立即清理子模块历史（git filter-branch 或 BFG）
- 严重违规需重新初始化子模块

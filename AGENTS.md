## 启动必读
* OpenCode 启动时必须读取并加载本文件 (AGENTS.md) 后再执行任何任务
* 读取 memory/里的文档，获取历史研究记录
* 如未读取到本文件，必须先加载本文件并遵循其内容，再继续执行
* RULE 为强制要求；如与系统/开发者指令冲突，以上级指令为准
* CODE STYLE 为强制要求；如与系统/开发者指令冲突，以上级指令为准
* 使用`local-docs-research`查找文档

# RULE
* 使用 `send-msg-to-usr` skill进行通知
* 运行 python 时，需要保留日志到 `./log/` 目录, 并查看日志内是否存在错误确认程序运行是否成功
* 测试程序放入./tests/, `./log`是存放日志的目录
* 执行任务时首先要检索文档，寻找合适的skill。
* 遵循责任最小原则，仅对本项目的工作负责，不修改其他项目，仅提出意见。
* 完成任务后整理git, 若关联了远程则推送

---

## role: PM (项目经理)

### 权限
* 可调用 Task tool 启动子 agent
* 可修改 AGENTS.md 任务状态
* 可创建和关闭 Plan

### 职责
* 对任务进行合理规划，要求每个todo不能过大
* 根据进展合理的调整规划
* 忠实完成所有的任务，不因优先级而请求，不因耗时而请求

### 自动化流程
1. **接收需求** →  定期从 `./plan` 里读取新任务, 格式为 ./plan/[current_plan].md
2. 查看issue  →  从 `./issues` 发现新 issue， 格式为./issues/[current_issue].md， 
    * 如果没有plan，分析 ./issues/[current_issue].md，创建新的plan，./plan/[current_issue].md,
    * 如果有plan, 分析 ./issues/[current_issue].md，指定新的plan，更新 ./plan/[current_plan].md
    * 把 ./issues/[current_issue].md 移动到 ./end_task/
2. **创建 Plan** →  更新 `./plan/[current_plan].md`里的状态
3. **启动执行** → 按依赖顺序启动子 agent
   - Task tool 调用 DEV agent
   - Task tool 调用 QA agent  
   - Task tool 调用 DOC agent
4. **验收交付** → 检查任务状态, 检查是否有新 issue
5. **归档** → 移动已完成`Plan`和`issue` 到 `./end_task/`
### 验收标准
1. 通过review
2. 没有新的issue

### 决策规则
* 任务失败 → 自动重新分配或降级
* 依赖阻塞 → 通知用户
* 优先级冲突 → 按紧急程度排序
* 发现新issue →  重新规划，分配任务
* 里程碑进展 →  通知用户

---
## role: ISSUE (需求报告)
### 职责
* 每分钟查看`./issues` 里是否有新的issue， 如果有，通知PM，在当前的plan文档加入
* 发生通知

---

## role: DEV (开发)

### 职责
* 检查文档，寻找合适的skill
* 代码实现 只修改本项目的代码
* 单元测试

### 输出格式 (更新到 ./plan/[current_plan].md)
```markdown
## DEV 交付报告
任务: [任务名称]
状态: [完成/部分完成/失败]
文件: [修改的文件列表]
测试: [单元测试结果]
备注: [需要 QA 注意的问题]
```

---

## role: QA (测试)

### 职责  
* 检索本机知识库，禁止重复造轮子
* 选择合适的skill进行code review
* 如果是python安装包，使用`py-package-review` skill进行安装包审查
* 对api的性能进行测试
* 边界检查、异常检查
### 输出格式 (更新到 ./plan/[current_plan].md)
```markdown
## QA 测试报告
任务: [任务名称]
状态: [通过/失败/阻塞]
测试覆盖: [百分比]
性能指标: [关键指标]
问题列表: [发现的 bug]
建议: [改进建议]
```

---

## role: DOC (文档)

### 职责
* README 维护
* 性能测试结果展示
* API 文档, 包括使用示例，参数说明
* 文档合理分类整理
* 使用发布文档的skill来发布文档，默认发布到本地


### 输出格式 (更新到 ./plan/[current_plan].md)
```markdown
## DOC 文档报告
任务: [任务名称]
状态: [完成/失败]
文档路径: [新增/修改的文档]
示例代码: [可运行的示例路径]
```

---

## role: ULW_PM (Build Agent)

> opencode plugin 级别的自动化编排 agent，不覆盖 opencode 内置的 Plan/Build 功能。

### 使用方式
* **Agent 模式**: 通过 Tab 切换到 `ulw_pm` agent，以 ULW_PM 角色对话，自动编排任务
* **Tool 模式**: 任何 agent 可调用 `ulw_pm` tool（action: `status` / `run` / `stop` / `context` / `compact`）

### 权限
* 可通过 opencode SDK (`client.session.create/prompt`) 创建和管理子 session
* 可读写 `./plan/` 和 `./issues/` 目录
* 可调用 `send-msg-to-usr` skill 通知用户
* 拥有 read/edit/bash/task/skill/webfetch/question 等工具权限

### 注册方式
* 通过插件的 `config` hook 注册为 `mode: "primary"` agent，Tab 可切换
* System prompt 定义在 `~/.config/opencode/plugins/ulw-pm/prompt.txt`

### 职责
* 读取 `./plan/[current_plan].md` 解析任务状态
* 检查 `TodoWrite` todos 获取未完成任务
* 合并两种来源，调度 DEV/QA/DOC/ISSUE 子 agent 执行任务
* 持续循环检查直到所有任务完成（自适应退避，事件驱动优先）
* 完成后执行 `store-memory` 并通知用户

### 触发方式
* **手动**: 调用 `ulw_pm` tool（action: `status` / `run` / `stop` / `context` / `compact`）
* **事件驱动**: 监听 `todo.updated` 和 `session.idle` 事件自动触发
* opencode 内置的 Plan/Build 功能保持不变，ULW_PM 不劫持、不覆盖

### 上下文监控
* 自动监控上下文使用率，超过阈值（默认 80%）自动压缩
* 使用 `context` action 查看当前上下文使用情况
* 使用 `compact` action 手动触发压缩
* 通过 `experimental.session.compacting` hook 保留任务上下文

### 配置参数 (ulw.toml)
项目根目录 `ulw.toml` 可配置，文件不存在时使用默认值:

| 参数 | 默认值 | 说明 |
|---|---|---|
| `max_cycles` | 50 | 最大循环次数 |
| `retry_limit` | 3 | 单任务最大重试次数 |
| `cycle_delay_min` | 30000 | 最小轮询间隔 (ms) |
| `cycle_delay_max` | 7200000 | 最大轮询间隔/退避上限 (ms, 2h) |
| `backoff_multiplier` | 2.0 | 退避倍数 |
| `auto_trigger` | true | 监听事件自动触发 |
| `event_driven` | true | 事件驱动优先，轮询仅兜底 |
| `compact.context_ratio` | 0.8 | 上下文压缩阈值 (0.0-1.0) |
| `compact.auto_compact` | true | 自动压缩 |
| `compact.summarize_model` | zai-coding-plan/glm-5.1 | 压缩使用的模型 |

### 插件文件位置
`~/.config/opencode/plugins/ulw-pm/`

### 输出格式 (更新到 ./plan/[current_plan].md)
```markdown
## ULW_PM Build 报告
Plan: [plan 名称]
状态: [运行中/已完成/失败]
进度: [n/m] 个任务
当前执行: [当前任务名称及 agent]
失败任务: [失败列表及原因]
下一轮计划: [待执行任务列表]
```

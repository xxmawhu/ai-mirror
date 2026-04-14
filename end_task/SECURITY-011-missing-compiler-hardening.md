---
id: SECURITY-011
severity: LOW
cvss: 2.3
status: new
createdAt: 2026-04-14
component: build
---

# SECURITY-011: 缺少编译器和链接器安全加固标志

## 严重级别
**LOW - 纵深防御缺失**

## CVSS 评分
2.3 (AV:L/AC:H/PR:H/UI:N/S:U/C:N/I:N/A:L)

## 发现位置
- `CMakeLists.txt:36-38`

## 漏洞描述

当前编译选项仅包含警告标志，缺少所有标准的安全加固标志：

```cmake
# CMakeLists.txt:36-38
target_compile_options(ai-mirror PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
```

缺少的加固措施：
- **Position Independent Executable (PIE/ASLR)**：`-fPIE -pie`
- **Stack Canary**：`-fstack-protector-strong`
- **Source Fortification**：`-D_FORTIFY_SOURCE=2`
- **Read-only RELRO**：`-Wl,-z,relro,-z,now`
- **GOT 隐藏**：`-fvisibility=hidden`
- **格式化字符串保护**：`-Wformat -Wformat-security`
- **不绑定前缀**：`-fpie`

对于以 root 权限运行（sudo）、执行外部命令（mount/useradd）、处理用户输入（路径/配置）的安全工具，这些加固措施是基本要求。

## 影响
- 潜在的内存破坏漏洞（栈溢出、格式化字符串等）可被更容易利用
- 如果代码中存在缓冲区溢出，缺少 stack canary 使其更容易被利用
- 没有 PIE/ASLR，攻击者可预测代码地址

## 修复建议

```cmake
target_compile_options(ai-mirror PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -Wformat -Wformat-security
    -fvisibility=hidden
)

target_link_options(ai-mirror PRIVATE
    -pie -Wl,-z,relro,-Wl,-z,now -Wl,-z,noexecstack
)
```

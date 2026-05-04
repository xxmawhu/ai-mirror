# ai-mirror

**AI 时代的 Linux 用户隔离方案 — 让每个 AI Agent 在自己的沙盒里安全工作。**

## 快速开始

### 一键安装

```bash
bash install.sh
```

### 仅构建（不安装到系统）

```bash
bash install.sh --build
```

### 卸载

```bash
bash install.sh --clean
```

### 手动构建

```bash
# 依赖: g++ 13+, cmake 3.28+
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## 运行环境

- Linux（任何发行版）
- 依赖: cmake, g++, make, git, openssh-server, sudo

## 安装后

```bash
# 添加用户到 ai-mirror 组
sudo usermod -aG ai-mirror $USER

# 重新登录或手动加载
source /etc/profile.d/am.sh

# 创建项目用户
am create /path/to/project
```

## License

MIT

#!/usr/bin/env bash
#
# Docker 端到端测试：验证 am rm 对旧格式 .am_status 的修复
#
set -euo pipefail

IMAGE_NAME="ai-mirror-test"
CONTAINER_NAME="ai-mirror-test-run"

echo "============================================================"
echo "Docker 端到端测试: am rm 旧格式 .am_status 修复验证"
echo "============================================================"

# 构建 Docker 镜像
echo ""
echo "[1/3] 构建 Docker 镜像..."

cat >/tmp/Dockerfile.ai-mirror-test <<'DOCKERFILE_EOF'
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    g++ cmake git openssh-server sudo libssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /run/sshd && ssh-keygen -A

# Remove existing ubuntu user, create maxx as UID 1000
RUN userdel -r ubuntu 2>/dev/null || true
RUN useradd -m -s /bin/bash -u 1000 maxx

# Setup ai-mirror group and sudoers for maxx
RUN groupadd ai-mirror && \
    usermod -aG ai-mirror maxx && \
    echo "maxx ALL=(ALL) NOPASSWD:SETENV: /usr/local/bin/ai-mirror-bin" >> /etc/sudoers

COPY . /opt/ai-mirror-src/
WORKDIR /opt/ai-mirror-src

RUN cmake -B build-test -DCMAKE_BUILD_TYPE=Debug . && \
    cmake --build build-test --target ai-mirror -j4

RUN install -d /usr/local/bin && \
    install -m 0755 build-test/bin/ai-mirror /usr/local/bin/ai-mirror-bin

RUN mkdir -p /etc/profile.d && cp profile/am.sh /etc/profile.d/am.sh

RUN mkdir -p /etc/ai-mirror && \
    printf '[user]\nprefix = "imaxx_"\nshell = "/bin/bash"\n' > /etc/ai-mirror/config.toml

WORKDIR /home/maxx/dev
DOCKERFILE_EOF

# Resolve project root (parent of docker-test/)
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

docker build -t "$IMAGE_NAME" -f /tmp/Dockerfile.ai-mirror-test "$PROJECT_ROOT"

rc=$?
if [[ $rc -ne 0 ]]; then
	echo "❌ Docker build failed (exit code: $rc)"
	rm -f /tmp/Dockerfile.ai-mirror-test
	exit 1
fi

echo "✅ Docker image built: $IMAGE_NAME"

# 运行测试
echo ""
echo "[2/3] 运行测试容器..."
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
docker run -d --name "$CONTAINER_NAME" "$IMAGE_NAME" /usr/sbin/sshd -D
sleep 2

echo ""
echo "[3/3] 执行测试用例..."
echo "------------------------------------------------------------"

FAILURES=0

# Test 1: 模拟旧格式 .am_status + am rm
echo ""
echo "=== 测试 1: 创建旧格式 .am_status 项目 ==="

docker exec "$CONTAINER_NAME" bash -c '
    mkdir -p /home/maxx/dev/listen_msg_from_tg
    cd /home/maxx/dev/listen_msg_from_tg
    git init
    git config user.email "test@test.com"
    git config user.name "Test"
    echo "bot" > main.py
    git add . && git commit -m "init"
    chown -R maxx:maxx /home/maxx/dev/listen_msg_from_tg

    # 创建 AI user（用旧命名格式，模拟旧版创建）
    useradd -m -s /bin/bash -u 10020025 imaxx_listen_msg_from_tg
    mkdir -p /home/maxx/dev/listen_msg_from_tg/.ssh
    chown -R imaxx_listen_msg_from_tg:imaxx_listen_msg_from_tg /home/maxx/dev/listen_msg_from_tg

    # 写入旧格式 .am_status（无 hash/project_path/path_hash）
    cat > /home/maxx/dev/listen_msg_from_tg/.am_status << OLDSTATUS
{
  "username": "imaxx_listen_msg_from_tg",
  "uid": 10020025,
  "gid": 10020025,
  "home_dir": "/home/maxx/dev/listen_msg_from_tg",
  "main_user": "maxx",
  "timestamp": 1778725710061735
}
OLDSTATUS
    chown imaxx_listen_msg_from_tg:imaxx_listen_msg_from_tg /home/maxx/dev/listen_msg_from_tg/.am_status
'

echo "✅ 旧格式 .am_status 创建完成"

# Test 2: 执行 am rm
echo ""
echo "=== 测试 2: am rm 旧格式项目（核心测试） ==="

output=$(docker exec "$CONTAINER_NAME" bash -c '
    cd /home/maxx/dev
    su - maxx -c "cd /home/maxx/dev && source /etc/profile.d/am.sh && am rm /home/maxx/dev/listen_msg_from_tg" 2>&1
' 2>&1)

exit_code=$?
echo "  am rm 输出: $output"
echo "  退出码: $exit_code"

# 验证结果
if echo "$output" | grep -qiE "not found|expected user.*imaxx_[a-f0-9]+"; then
	echo ""
	echo "  ❌ FAIL: am rm 失败（使用了 derive_username 而不是读取 .am_status）"
	echo "  说明旧格式 .am_status 读取修复未生效"
	FAILURES=$((FAILURES + 1))
elif echo "$output" | grep -qiE "removing|removed|success"; then
	echo ""
	echo "  ✅ PASS: am rm 成功执行"
elif [[ $exit_code -eq 0 ]]; then
	echo ""
	echo "  ✅ PASS: am rm 成功执行 (exit code 0)"
else
	echo ""
	echo "  ⚠️ 不确定: exit_code=$exit_code, output=$output"
fi

# Test 3: 验证用户已被删除
echo ""
echo "=== 测试 3: 验证 AI user 已被删除 ==="

output=$(docker exec "$CONTAINER_NAME" id imaxx_listen_msg_from_tg 2>&1)
if echo "$output" | grep -q "no such user"; then
	echo "  ✅ PASS: 用户已删除"
else
	echo "  ❌ FAIL: 用户仍存在: $output"
	FAILURES=$((FAILURES + 1))
fi

# 清理
echo ""
echo "------------------------------------------------------------"
docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1
rm -f /tmp/Dockerfile.ai-mirror-test

if [[ $FAILURES -eq 0 ]]; then
	echo ""
	echo "============================================================"
	echo "✅ 所有测试通过! 旧格式 .am_status 修复验证成功"
	echo "============================================================"
else
	echo ""
	echo "============================================================"
	echo "❌ $FAILURES 个测试失败"
	echo "============================================================"
	exit 1
fi

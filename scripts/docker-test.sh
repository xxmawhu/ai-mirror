#!/usr/bin/env bash
# ai-mirror Docker 测试套件
# 一键运行所有测试层，确保安装成功
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="${SCRIPT_DIR}/log/docker-test"
mkdir -p "$LOG_DIR"
TIMESTAMP=$(date '+%Y%m%d-%H%M%S')
PASS=0
FAIL=0

ok() {
	echo -e "  \033[32m[PASS]\033[0m $1"
	PASS=$((PASS + 1))
}
fail() {
	echo -e "  \033[31m[FAIL]\033[0m $1"
	FAIL=$((FAIL + 1))
}

# ============================================
# Layer 1: Pre-commit Hook Check
# ============================================
layer_precommit() {
	echo ""
	echo "============================================"
	echo "Layer 1: Pre-commit Hook Check"
	echo "============================================"

	# 1.1 shellcheck on install.sh
	echo "  [1.1] shellcheck install.sh..."
	if pre-commit run shellcheck --files install.sh 2>&1 | grep -q "Passed"; then
		ok "install.sh shellcheck passed"
	else
		fail "install.sh shellcheck FAILED"
	fi

	# 1.2 shfmt on install.sh
	echo "  [1.2] shfmt install.sh..."
	if pre-commit run shfmt --files install.sh 2>&1 | grep -q "Passed"; then
		ok "install.sh shfmt passed"
	else
		fail "install.sh shfmt FAILED"
	fi

	# 1.3 Full pre-commit on all changed files
	echo "  [1.3] full pre-commit..."
	if pre-commit run --all-files 2>&1 | grep -q "Failed"; then
		fail "pre-commit has failures (check above)"
	else
		ok "full pre-commit passed"
	fi
}

# ============================================
# Layer 2: Build + Unit Tests
# ============================================
layer_build() {
	echo ""
	echo "============================================"
	echo "Layer 2: Build + Unit Tests"
	echo "============================================"

	# 2.1 CMake build
	echo "  [2.1] CMake build..."
	BUILD_DIR="${SCRIPT_DIR}/build-docker-test"
	cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >>"$LOG_DIR/build.${TIMESTAMP}.log" 2>&1
	cmake --build "$BUILD_DIR" -j"$(nproc)" >>"$LOG_DIR/build.${TIMESTAMP}.log" 2>&1
	ok "build complete"

	# 2.2 Test compilation
	echo "  [2.2] test targets..."
	cmake --build "$BUILD_DIR" --target test_mount_stat >>"$LOG_DIR/build.${TIMESTAMP}.log" 2>&1
	cmake --build "$BUILD_DIR" --target test_watch_stats >>"$LOG_DIR/build.${TIMESTAMP}.log" 2>&1
	cmake --build "$BUILD_DIR" --target test_known_hosts >>"$LOG_DIR/build.${TIMESTAMP}.log" 2>&1
	ok "test targets built"

	# 2.3 Run unit tests
	echo "  [2.3] unit tests..."
	for test_bin in test_mount_stat test_watch_stats test_known_hosts; do
		if "${BUILD_DIR}/bin/$test_bin" >>"$LOG_DIR/test.${TIMESTAMP}.log" 2>&1; then
			ok "$test_bin passed"
		else
			fail "$test_bin FAILED"
		fi
	done
}

# ============================================
# Layer 3: Install.sh Verification (Docker)
# ============================================
layer_install_docker() {
	echo ""
	echo "============================================"
	echo "Layer 3: Docker Install Verification"
	echo "============================================"

	# Build Docker image for install test
	echo "  [3.1] Building install-test Docker image..."
	cat >/tmp/Dockerfile.install-test <<'DOCKERFILE'
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    g++ cmake make git pkg-config \
    libssl-dev \
    openssh-server sudo \
    && rm -rf /var/lib/apt/lists/*
RUN groupadd -f ai-mirror && usermod -aG ai-mirror root
RUN useradd -m -s /bin/bash testuser && usermod -aG sudo testuser
WORKDIR /build
COPY . .
RUN echo "=== Layer 3.1: install.sh syntax check ===" && \
    bash -n install.sh && echo "Syntax OK"
RUN echo "=== Layer 3.2: cmake configure + build ===" && \
    cmake -B build-release -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build-release -j$(nproc) && \
    echo "Build OK"
RUN echo "=== Layer 3.3: install.sh install ===" && \
    bash install.sh && \
    echo "Install OK"
RUN echo "=== Layer 3.4: binary verification ===" && \
    test -x /usr/local/bin/am && \
    test -x /usr/local/bin/ai-mirror-bin && \
    test -x /usr/local/bin/am-mount-watch && \
    /usr/local/bin/am --help >/dev/null 2>&1 && \
    echo "Binary verification OK"
RUN echo "=== Layer 3.5: pre-commit check ===" && \
    pip3 install pre-commit 2>&1 | tail -1 && \
    pre-commit install 2>&1 | tail -1 && \
    pre-commit run --all-files 2>&1 || true
CMD ["bash", "-c", "echo 'Install test completed successfully'; /usr/local/bin/am --help"]
DOCKERFILE

	if docker build -t ai-mirror-install-test -f /tmp/Dockerfile.install-test "$SCRIPT_DIR" \
		>>"$LOG_DIR/docker.${TIMESTAMP}.log" 2>&1; then
		ok "Docker install test PASSED"
	else
		fail "Docker install test FAILED (check ${LOG_DIR}/docker.${TIMESTAMP}.log)"
		echo ""
		echo "  Last 20 lines of Docker build log:"
		tail -20 "$LOG_DIR/docker.${TIMESTAMP}.log" | sed 's/^/    /'
	fi
}

# ============================================
# Layer 4: Docker Integration Tests
# ============================================
layer_integration_docker() {
	echo ""
	echo "============================================"
	echo "Layer 4: Docker Integration Tests"
	echo "============================================"

	# Use existing Dockerfile.test
	echo "  [4.1] Building test Docker image..."
	# We need to add the new test file to the build context
	# Use the existing tests/docker/Dockerfile.test
	if docker build -t ai-mirror-integration-test \
		-f tests/docker/Dockerfile.test "$SCRIPT_DIR" \
		>>"$LOG_DIR/integration.${TIMESTAMP}.log" 2>&1; then
		# Run the test
		echo "  [4.2] Running integration tests..."
		if docker run --rm --privileged ai-mirror-integration-test \
			>>"$LOG_DIR/integration-run.${TIMESTAMP}.log" 2>&1; then
			ok "integration tests passed"
		else
			fail "integration tests FAILED (check ${LOG_DIR}/integration-run.${TIMESTAMP}.log)"
			echo ""
			echo "  Last 30 lines:"
			tail -30 "$LOG_DIR/integration-run.${TIMESTAMP}.log" | sed 's/^/    /'
		fi
	else
		fail "integration test build FAILED (check ${LOG_DIR}/integration.${TIMESTAMP}.log)"
		tail -20 "$LOG_DIR/integration.${TIMESTAMP}.log" | sed 's/^/    /'
	fi
}

# ============================================
# Main
# ============================================
main() {
	echo "============================================"
	echo " ai-mirror Docker Test Suite"
	echo " Timestamp: ${TIMESTAMP}"
	echo " Log dir:   ${LOG_DIR}"
	echo "============================================"

	# Determine which layers to run
	local layers="${1:-all}"

	case "$layers" in
	all)
		layer_precommit
		layer_build
		layer_install_docker
		layer_integration_docker
		;;
	precommit | 1)
		layer_precommit
		;;
	build | 2)
		layer_build
		;;
	install | 3)
		layer_install_docker
		;;
	integration | 4)
		layer_integration_docker
		;;
	*)
		echo "Usage: $0 {all|precommit|build|install|integration}"
		echo "  all          Run all layers (default)"
		echo "  precommit    Layer 1: pre-commit hooks"
		echo "  build        Layer 2: build + unit tests"
		echo "  install      Layer 3: Docker install test"
		echo "  integration  Layer 4: Docker integration tests"
		exit 1
		;;
	esac

	echo ""
	echo "============================================"
	echo " Results: $PASS passed, $FAIL failed"
	echo "============================================"

	if [ $FAIL -gt 0 ]; then
		echo -e "\033[31mFAILED\033[0m"
		exit 1
	fi
	echo -e "\033[32mALL TESTS PASSED\033[0m"
}

main "$@"

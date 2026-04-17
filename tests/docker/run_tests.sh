#!/bin/bash
set -e

echo "========================================"
echo "ai-mirror Docker Integration Tests"
echo "========================================"

LOG_DIR="/var/log/ai-mirror-tests"
mkdir -p "$LOG_DIR"

pass_count=0
fail_count=0

run_test() {
	local name="$1"
	local cmd="$2"
	local expected="$3"
	local pattern="${4:-}"

	echo ""
	echo "=== Test: $name ==="
	echo "Command: $cmd"

	local output
	local exit_code

	output=$(eval "$cmd" 2>&1) && exit_code=0 || exit_code=$?

	echo "Exit code: $exit_code"
	echo "Output (last 5 lines):"
	echo "$output" | tail -5

	if [[ "$expected" == "success" ]]; then
		if [[ $exit_code -eq 0 ]]; then
			echo "[PASS] $name"
			((pass_count++))
		else
			echo "[FAIL] $name - expected success, got exit code $exit_code"
			((fail_count++))
		fi
	elif [[ "$expected" == "fail" ]]; then
		if [[ $exit_code -ne 0 ]]; then
			echo "[PASS] $name (correctly failed)"
			((pass_count++))
		else
			echo "[FAIL] $name - expected failure, got success"
			((fail_count++))
		fi
	elif [[ "$expected" == "contains" ]]; then
		if [[ "$output" == *"$pattern"* ]]; then
			echo "[PASS] $name - contains '$pattern'"
			((pass_count++))
		else
			echo "[FAIL] $name - expected to contain '$pattern'"
			((fail_count++))
		fi
	fi

	echo "$name: exit=$exit_code output=$output" >>"$LOG_DIR/test_results.log"
}

setup_testuser_env() {
	export HOME=/home/testuser
	testuid=$(id -u testuser)
	echo "$testuid" >/proc/self/loginuid 2>/dev/null || true
}

setup_datauser_env() {
	export HOME=/data/home/datauser
	datauid=$(id -u datauser)
	echo "$datauid" >/proc/self/loginuid 2>/dev/null || true
}

setup_ssh_for_testuser() {
	rm -f /home/testuser/.ssh/id_ed25519 /home/testuser/.ssh/id_ed25519.pub
	ssh-keygen -t ed25519 -f /home/testuser/.ssh/id_ed25519 -N "" -C "testuser" || true
	chown -R testuser:testuser /home/testuser/.ssh
}

setup_ssh_for_datauser() {
	rm -f /data/home/datauser/.ssh/id_ed25519 /data/home/datauser/.ssh/id_ed25519.pub
	ssh-keygen -t ed25519 -f /data/home/datauser/.ssh/id_ed25519 -N "" -C "datauser" || true
	chown -R datauser:datauser /data/home/datauser/.ssh
}

echo ""
echo "=== Setting up SSH keys ==="
setup_ssh_for_testuser
setup_ssh_for_datauser

echo ""
echo "========================================"
echo "Scenario 1: Standard HOME (/home/user)"
echo "========================================"

mkdir -p /home/testuser/projects/project1
chown -R testuser:testuser /home/testuser/projects
setup_testuser_env

run_test "1.1: Create ai-user (standard HOME)" \
	"/usr/local/bin/am create /home/testuser/projects/project1" \
	"success"

run_test "1.2: List ai-users" \
	"/usr/local/bin/am list" \
	"contains" "imaxx_"

run_test "1.3: Remove ai-user" \
	"/usr/local/bin/am rm /home/testuser/projects/project1" \
	"success"

run_test "1.4: Verify user removed" \
	"id imaxx_project1 2>&1; true" \
	"contains" "no such user"

echo ""
echo "========================================"
echo "Scenario 2: Custom HOME (beegfs style)"
echo "========================================"

mkdir -p /data/home/datauser/projects/project2
chown -R datauser:datauser /data/home/datauser/projects
setup_datauser_env
setup_ssh_for_datauser

run_test "2.1: Create ai-user (custom HOME)" \
	"/usr/local/bin/am create /data/home/datauser/projects/project2" \
	"success"

run_test "2.2: Verify ai-user home is project path" \
	"getent passwd imaxx_project2 | cut -d: -f6" \
	"contains" "/data/home/datauser/projects/project2"

run_test "2.3: Remove ai-user (custom HOME)" \
	"/usr/local/bin/am rm /data/home/datauser/projects/project2" \
	"success"

echo ""
echo "========================================"
echo "Scenario 3: SSH Setup"
echo "========================================"

mkdir -p /home/testuser/projects/project3
chown -R testuser:testuser /home/testuser/projects/project3
setup_testuser_env
setup_ssh_for_testuser

run_test "3.1: Create ai-user with SSH" \
	"/usr/local/bin/am create /home/testuser/projects/project3" \
	"success"

run_test "3.2: Check .ssh directory in project" \
	"ls -la /home/testuser/projects/project3/.ssh" \
	"success"

run_test "3.3: Check authorized_keys" \
	"cat /home/testuser/projects/project3/.ssh/authorized_keys" \
	"contains" "ssh-ed25519"

run_test "3.4: Remove ai-user" \
	"/usr/local/bin/am rm /home/testuser/projects/project3" \
	"success"

echo ""
echo "========================================"
echo "Scenario 4: Error Handling"
echo "========================================"

setup_testuser_env

run_test "4.1: Create outside home (should fail)" \
	"/usr/local/bin/am create /tmp/illegal_project" \
	"fail"

run_test "4.2: Create in system dir (should fail)" \
	"/usr/local/bin/am create /etc/illegal" \
	"fail"

run_test "4.3: Remove non-existent user (should fail)" \
	"/usr/local/bin/am rm /home/testuser/projects/nonexistent" \
	"fail"

echo ""
echo "========================================"
echo "Scenario 5: Multiple Projects"
echo "========================================"

mkdir -p /home/testuser/projects/multi1
mkdir -p /home/testuser/projects/multi2
mkdir -p /home/testuser/projects/multi3
chown -R testuser:testuser /home/testuser/projects
setup_testuser_env

run_test "5.1: Create project multi1" \
	"/usr/local/bin/am create /home/testuser/projects/multi1" \
	"success"

run_test "5.2: Create project multi2" \
	"/usr/local/bin/am create /home/testuser/projects/multi2" \
	"success"

run_test "5.3: Create project multi3" \
	"/usr/local/bin/am create /home/testuser/projects/multi3" \
	"success"

run_test "5.4: List shows all 3 projects" \
	"/usr/local/bin/am list | grep -c 'imaxx_multi'" \
	"contains" "3"

run_test "5.5: Remove all projects" \
	"/usr/local/bin/am rm /home/testuser/projects/multi1 && \
     /usr/local/bin/am rm /home/testuser/projects/multi2 && \
     /usr/local/bin/am rm /home/testuser/projects/multi3" \
	"success"

echo ""
echo "========================================"
echo "Test Summary"
echo "========================================"
echo "Passed: $pass_count"
echo "Failed: $fail_count"
echo ""

if [[ $fail_count -eq 0 ]]; then
	echo "All tests passed!"
	exit 0
else
	echo "Some tests failed!"
	exit 1
fi

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

	echo "Exit code: $exit_code" >&2
	echo "Output (last 5 lines):" >&2
	echo "$output" | tail -5 >&2

	if [[ "$expected" == "success" ]]; then
		if [[ $exit_code -eq 0 ]]; then
			echo "[PASS] $name"
			pass_count=$((pass_count + 1))
		else
			echo "[FAIL] $name - expected success, got exit code $exit_code"
			fail_count=$((fail_count + 1))
		fi
	elif [[ "$expected" == "fail" ]]; then
		if [[ $exit_code -ne 0 ]]; then
			echo "[PASS] $name (correctly failed)"
			pass_count=$((pass_count + 1))
		else
			echo "[FAIL] $name - expected failure, got success"
			fail_count=$((fail_count + 1))
		fi
	elif [[ "$expected" == "contains" ]]; then
		if [[ "$output" == *"$pattern"* ]]; then
			echo "[PASS] $name - contains '$pattern'"
			pass_count=$((pass_count + 1))
		else
			echo "[FAIL] $name - expected to contain '$pattern'"
			fail_count=$((fail_count + 1))
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
echo "Scenario 0: Install Verification"
echo "========================================"

run_test "0.1: /etc/profile.d/am.sh syntax check" \
	"bash -n /etc/profile.d/am.sh" \
	"success"

run_test "0.2: /etc/profile.d/am.sh has no orphan ')'" \
	"bash -c 'source /etc/profile.d/am.sh && echo OK'" \
	"contains" "OK"

run_test "0.3: am binary is executable" \
	"test -x /usr/local/bin/am" \
	"success"

run_test "0.4: ai-mirror-bin symlink exists and is executable" \
	"test -x /usr/local/bin/ai-mirror-bin" \
	"success"

run_test "0.5: install.sh itself passes syntax check" \
	"bash -n /app/install.sh" \
	"success"

run_test "0.6: profile/am.sh source has no syntax error" \
	"bash -n /app/profile/am.sh" \
	"success"

run_test "0.7: bash_completion is readable" \
	"test -r /etc/bash_completion.d/am" \
	"success"

run_test "0.8: am --help returns success" \
	"/usr/local/bin/am --help" \
	"success"

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
	"contains" "itestuser_"

run_test "1.3: Remove ai-user" \
	"/usr/local/bin/am rm /home/testuser/projects/project1" \
	"success"

run_test "1.4: Verify user removed" \
	"id itestuser_project1 2>&1; true" \
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

custom_user=$(getent passwd | grep '^idatauser_' | cut -d: -f1 | head -1)

run_test "2.2: Verify ai-user home is project path" \
	"getent passwd $custom_user | cut -d: -f6" \
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
chown -R testuser:testuser /home/testuser/projects || true
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
	"/usr/local/bin/am list | grep -c 'itestuser_multi'" \
	"contains" "3"

run_test "5.5: Remove all projects" \
	"/usr/local/bin/am rm /home/testuser/projects/multi1 && \
     /usr/local/bin/am rm /home/testuser/projects/multi2 && \
     /usr/local/bin/am rm /home/testuser/projects/multi3" \
	"success"

echo ""
echo "========================================"
echo "Scenario 6: Config and Status Commands"
echo "========================================"

setup_testuser_env

run_test "6.1: Config command" \
	"/usr/local/bin/am config" \
	"contains" "Config file"

mkdir -p /home/testuser/projects/proj_config
chown -R testuser:testuser /home/testuser/projects || true

run_test "6.2: Create project for status test" \
	"/usr/local/bin/am create /home/testuser/projects/proj_config" \
	"success"

run_test "6.3: Status command shows project" \
	"/usr/local/bin/am status" \
	"contains" "Project:"

run_test "6.4: Status shows SSH info" \
	"/usr/local/bin/am status" \
	"contains" "SSH:"

run_test "6.5: Cleanup config test" \
	"/usr/local/bin/am rm /home/testuser/projects/proj_config" \
	"success"

echo ""
echo "========================================"
echo "Scenario 7: ai-user File Ownership"
echo "========================================"

setup_testuser_env
mkdir -p /home/testuser/projects/ownertest
chown -R testuser:testuser /home/testuser/projects || true

run_test "7.1: Create ai-user for ownership test" \
	"/usr/local/bin/am create /home/testuser/projects/ownertest" \
	"success"

owner_user=$(getent passwd | grep '^itestuser_.*ownertest' | cut -d: -f1 | head -1)

run_test "7.2: .ssh dir owned by ai-user" \
	"stat -c '%U' /home/testuser/projects/ownertest/.ssh" \
	"contains" "$owner_user"

run_test "7.3: authorized_keys owned by ai-user" \
	"stat -c '%U' /home/testuser/projects/ownertest/.ssh/authorized_keys" \
	"contains" "$owner_user"

run_test "7.4: Project dir has write grant" \
	"stat -c '%A' /home/testuser/projects/ownertest" \
	"contains" "rwx"

run_test "7.5: Cleanup ownership test" \
	"/usr/local/bin/am rm /home/testuser/projects/ownertest" \
	"success"

echo ""
echo "========================================"
echo "Scenario 7B: Update Ownership Fix"
echo "========================================"

setup_testuser_env
mkdir -p /home/testuser/projects/owntest2
chown -R testuser:testuser /home/testuser/projects || true

run_test "7B.1: Create project for update-ownership test" \
	"/usr/local/bin/am create /home/testuser/projects/owntest2" \
	"success"

owntest2_user=$(getent passwd | grep '^itestuser_.*owntest2' | cut -d: -f1 | head -1)

# Ownership rule: .am_status should be root, everything else should be ai-user or main-user (mount source)
run_test "7B.2: .am_status owned by root" \
	"stat -c '%U' /home/testuser/projects/owntest2/.am_status" \
	"contains" "root"

# Check .local ownership (intermediate dir should be ai-user, not root)
run_test "7B.3: .local dir owned by ai-user (not root)" \
	"stat -c '%U' /home/testuser/projects/owntest2/.local" \
	"contains" "$owntest2_user"

# Check .local/bin ownership (bind mount source owned by testuser)
run_test "7B.4: .local/bin owned by testuser (mount source)" \
	"stat -c '%U' /home/testuser/projects/owntest2/.local/bin" \
	"contains" "testuser"

# Check .bashrc ownership (bind mount source owned by testuser)
run_test "7B.5: .bashrc owned by testuser (mount source)" \
	"stat -c '%U' /home/testuser/projects/owntest2/.bashrc" \
	"contains" "testuser"

# Check .ssh ownership (ai-user's own dir)
run_test "7B.6: .ssh owned by ai-user" \
	"stat -c '%U' /home/testuser/projects/owntest2/.ssh" \
	"contains" "$owntest2_user"

# Run am update and verify ownership is preserved/fixed
run_test "7B.7: am update preserves ownership" \
	"/usr/local/bin/am update /home/testuser/projects/owntest2" \
	"success"

run_test "7B.8: .am_status still owned by root after update" \
	"stat -c '%U' /home/testuser/projects/owntest2/.am_status" \
	"contains" "root"

run_test "7B.9: .local still owned by ai-user after update" \
	"stat -c '%U' /home/testuser/projects/owntest2/.local" \
	"contains" "$owntest2_user"

run_test "7B.10: .local/bin still owned by testuser after update" \
	"stat -c '%U' /home/testuser/projects/owntest2/.local/bin" \
	"contains" "testuser"

run_test "7B.11: Cleanup owntest2" \
	"/usr/local/bin/am rm /home/testuser/projects/owntest2" \
	"success"

echo ""
echo "========================================"
echo "Scenario 8: Recreate Same Project"
echo "========================================"

setup_testuser_env
mkdir -p /home/testuser/projects/recreate
chown -R testuser:testuser /home/testuser/projects || true

run_test "8.1: Create project" \
	"/usr/local/bin/am create /home/testuser/projects/recreate" \
	"success"

recreate_user=$(getent passwd | grep '^itestuser_.*recreate' | cut -d: -f1 | head -1)

run_test "8.2: Remove project" \
	"/usr/local/bin/am rm /home/testuser/projects/recreate" \
	"success"

run_test "8.3: Verify user removed" \
	"id $recreate_user 2>&1; true" \
	"contains" "no such user"

setup_ssh_for_testuser

run_test "8.4: Recreate same project" \
	"/usr/local/bin/am create /home/testuser/projects/recreate" \
	"success"

run_test "8.5: Verify recreated user exists" \
	"getent passwd | grep '^itestuser_.*recreate'" \
	"contains" "itestuser_"

run_test "8.6: Cleanup recreate test" \
	"/usr/local/bin/am rm /home/testuser/projects/recreate" \
	"success"

echo ""
echo "========================================"
echo "Scenario 9: Path Edge Cases"
echo "========================================"

setup_testuser_env

mkdir -p "/home/testuser/projects/my-project"
chown -R testuser:testuser /home/testuser/projects || true

run_test "9.1: Create with hyphenated name" \
	"/usr/local/bin/am create /home/testuser/projects/my-project" \
	"success"

run_test "9.2: List shows hyphenated project" \
	"/usr/local/bin/am list" \
	"contains" "itestuser_"

run_test "9.3: Remove hyphenated project" \
	"/usr/local/bin/am rm /home/testuser/projects/my-project" \
	"success"

mkdir -p "/home/testuser/projects/deep/nested/project"
chown -R testuser:testuser /home/testuser/projects || true

run_test "9.4: Create deeply nested project" \
	"/usr/local/bin/am create /home/testuser/projects/deep/nested/project" \
	"success"

run_test "9.5: Remove deeply nested project" \
	"/usr/local/bin/am rm /home/testuser/projects/deep/nested/project" \
	"success"

echo ""
echo "========================================"
echo "Scenario 10: Duplicate Create Prevention"
echo "========================================"

setup_testuser_env
mkdir -p /home/testuser/projects/dup_test
chown -R testuser:testuser /home/testuser/projects || true

run_test "10.1: Create project" \
	"/usr/local/bin/am create /home/testuser/projects/dup_test" \
	"success"

run_test "10.2: Create same project again (should succeed, return existing)" \
	"/usr/local/bin/am create /home/testuser/projects/dup_test" \
	"success"

run_test "10.3: Verify only one ai-user exists for project" \
	"getent passwd | grep -c 'itestuser_.*dup_test'" \
	"contains" "1"

run_test "10.4: Cleanup dup test" \
	"/usr/local/bin/am rm /home/testuser/projects/dup_test" \
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

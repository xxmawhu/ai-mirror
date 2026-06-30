#!/usr/bin/env bash
# Docker integration test for am mv ownership security model
# Tests 5 scenarios: A, B, C, D, E

set -euo pipefail

IMAGE_NAME="ai-mirror-test"
CONTAINER_NAME="ai-mirror-test-container"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[test]${NC} $*"; }
info() { echo -e "${CYAN}[info]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
error() { echo -e "${RED}[error]${NC} $*" >&2; }

# Build Docker image
build_image() {
	log "Building Docker image..."
	docker build -t "$IMAGE_NAME" -f Dockerfile.test .
}

# Start container
start_container() {
	log "Starting container..."
	docker run -d --name "$CONTAINER_NAME" \
		--privileged \
		-v "$(pwd)/build-test/bin:/usr/local/bin:ro" \
		"$IMAGE_NAME" tail -f /dev/null

	# Wait for container to be ready
	sleep 2
}

# Stop and remove container
cleanup_container() {
	log "Cleaning up container..."
	docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
}

# Run test inside container
run_test() {
	local test_name="$1"
	local expected_owner="$2"
	local expected_exit_code="$3"
	local test_cmd="$4"

	info "Running test: $test_name"

	local result
	result=$(docker exec "$CONTAINER_NAME" bash -c "$test_cmd" 2>&1) || true
	local exit_code=$?

	# Get file owner
	local owner
	owner=$(docker exec "$CONTAINER_NAME" bash -c 'stat -c "%U" /tmp/testfile_moved' 2>&1) || true

	# Verify
	if [[ "$expected_exit_code" == "reject" ]]; then
		if [[ "$exit_code" -ne 0 ]]; then
			log "✅ $test_name: Correctly rejected (exit=$exit_code)"
			return 0
		else
			error "❌ $test_name: Should have been rejected but succeeded"
			return 1
		fi
	else
		if [[ "$exit_code" -eq 0 ]]; then
			if [[ "$owner" == "$expected_owner" ]]; then
				log "✅ $test_name: Owner=$owner (expected=$expected_owner)"
				return 0
			else
				error "❌ $test_name: Owner=$owner (expected=$expected_owner)"
				return 1
			fi
		else
			error "❌ $test_name: Failed with exit=$exit_code"
			error "Output: $result"
			return 1
		fi
	fi
}

# Setup test users and directories
setup_test_env() {
	log "Setting up test environment..."

	docker exec "$CONTAINER_NAME" bash -c '
        # Create main users
        useradd -m user1 || true
        useradd -m user2 || true

        # Create ai-users for user1
        useradd -m -u 10000001 i_user1_project1 || true
        useradd -m -u 10000002 i_user1_project2 || true

        # Create ai-user for user2
        useradd -m -u 20000001 i_user2_project1 || true

        # Setup directories
        mkdir -p /home/user1/sim_match
        mkdir -p /home/user1/project1
        mkdir -p /home/user1/project2
        mkdir -p /home/user2/project1

        # Create test files with various owners
        echo "test content" > /tmp/testfile_main
        chown user1:user1 /tmp/testfile_main

        echo "test content" > /home/i_user1_project1/testfile_ai1
        chown i_user1_project1:i_user1_project1 /home/i_user1_project1/testfile_ai1

        echo "test content" > /home/i_user1_project2/testfile_ai2
        chown i_user1_project2:i_user1_project2 /home/i_user1_project2/testfile_ai2

        echo "test content" > /home/i_user2_project1/testfile_other_ai
        chown i_user2_project1:i_user2_project1 /home/i_user2_project1/testfile_other_ai

        # Set ai-user directory ownership
        chown i_user1_project1:i_user1_project1 /home/i_user1_project1
        chown i_user1_project2:i_user1_project2 /home/i_user1_project2
        chown i_user2_project1:i_user2_project1 /home/i_user2_project1
        chown user1:user1 /home/user1/sim_match

        # Setup .am_status files (minimal)
        mkdir -p /var/lib/ai-mirror
        groupadd ai-mirror || true
        usermod -aG ai-mirror user1 || true
        usermod -aG ai-mirror user2 || true
    '
}

# Test scenarios
run_all_tests() {
	local failed=0

	# Scenario A: main-user -> ai-user
	log "=== Scenario A: main-user -> ai-user ==="
	docker exec "$CONTAINER_NAME" bash -c '
        cp /tmp/testfile_main /tmp/testfile_A
        chown user1:user1 /tmp/testfile_A
        chown i_user1_project1:i_user1_project1 /home/i_user1_project1
    '
	run_test "A: main->ai" "i_user1_project1" 0 '
        /usr/local/bin/ai-mirror mv /tmp/testfile_A /home/i_user1_project1/ 2>&1
        mv /home/i_user1_project1/testfile_A /tmp/testfile_moved 2>/dev/null || true
    ' || failed=$((failed + 1))

	# Scenario B: ai-user-A -> ai-user-B (same main-user)
	log "=== Scenario B: ai-user-A -> ai-user-B (same main) ==="
	docker exec "$CONTAINER_NAME" bash -c '
        cp /home/i_user1_project1/testfile_ai1 /tmp/testfile_B
        chown i_user1_project1:i_user1_project1 /tmp/testfile_B
        chown i_user1_project2:i_user1_project2 /home/i_user1_project2
    '
	run_test "B: ai->ai(same)" "i_user1_project2" 0 '
        /usr/local/bin/ai-mirror mv /tmp/testfile_B /home/i_user1_project2/ 2>&1
        mv /home/i_user1_project2/testfile_B /tmp/testfile_moved 2>/dev/null || true
    ' || failed=$((failed + 1))

	# Scenario C: ai-user -> main-user
	log "=== Scenario C: ai-user -> main-user ==="
	docker exec "$CONTAINER_NAME" bash -c '
        cp /home/i_user1_project1/testfile_ai1 /tmp/testfile_C
        chown i_user1_project1:i_user1_project1 /tmp/testfile_C
    '
	run_test "C: ai->main" "user1" 0 '
        /usr/local/bin/ai-mirror mv /tmp/testfile_C /home/user1/sim_match/ 2>&1
        mv /home/user1/sim_match/testfile_C /tmp/testfile_moved 2>/dev/null || true
    ' || failed=$((failed + 1))

	# Scenario D: ai-user(main1) -> ai-user(main2) [cross-user]
	log "=== Scenario D: ai-user(main1) -> ai-user(main2) [should reject] ==="
	docker exec "$CONTAINER_NAME" bash -c '
        cp /home/i_user1_project1/testfile_ai1 /tmp/testfile_D
        chown i_user1_project1:i_user1_project1 /tmp/testfile_D
        chown i_user2_project1:i_user2_project1 /home/i_user2_project1
    '
	run_test "D: cross-main" "" "reject" '
        /usr/local/bin/ai-mirror mv /tmp/testfile_D /home/i_user2_project1/ 2>&1
    ' || failed=$((failed + 1))

	# Scenario E: main-user -> main-user
	log "=== Scenario E: main-user -> main-user ==="
	docker exec "$CONTAINER_NAME" bash -c '
        cp /tmp/testfile_main /tmp/testfile_E1
        chown user1:user1 /tmp/testfile_E1
        mkdir -p /home/user1/dest_dir
        chown user1:user1 /home/user1/dest_dir
    '
	run_test "E: main->main" "user1" 0 '
        /usr/local/bin/ai-mirror mv /tmp/testfile_E1 /home/user1/dest_dir/ 2>&1
        mv /home/user1/dest_dir/testfile_E1 /tmp/testfile_moved 2>/dev/null || true
    ' || failed=$((failed + 1))

	return $failed
}

# Main
main() {
	cleanup_container
	build_image
	start_container
	setup_test_env

	local result
	if run_all_tests; then
		log "All tests passed!"
		result=0
	else
		error "Some tests failed"
		result=1
	fi

	cleanup_container
	exit $result
}

main "$@"

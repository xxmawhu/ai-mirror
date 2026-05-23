#!/usr/bin/env bash
#
# ai-mirror installer
# Builds from source and installs ai-mirror CLI.
#
# Usage:
#   bash install.sh            # full install (build + deploy, sudo used only when needed)
#   bash install.sh --build    # build only, no sudo needed
#   bash install.sh --clean    # remove installed files (sudo needed)
#
set -euo pipefail

# ---- Configuration ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-release"
LOG_DIR="${SCRIPT_DIR}/log"
INSTALL_LOG="${LOG_DIR}/install.log"

PREFIX="${AI_MIRROR_PREFIX:-/usr/local}"
CONFIG_DIR="${AI_MIRROR_CONFIG_DIR:-/etc/ai-mirror}"
DATA_DIR="${AI_MIRROR_DATA_DIR:-/var/lib/ai-mirror}"
BIN_NAME="ai-mirror"
REAL_BIN_NAME="ai-mirror-bin"

# ---- Error Tracking ----
CURRENT_PHASE="init"
ERROR_MSG=""

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ---- Helpers ----
_log() {
	local ts
	ts=$(date '+%Y-%m-%d %H:%M:%S')
	local msg="${ts} $1"
	echo -e "$msg"
	[[ -n "${LOG_DIR_READY:-}" ]] && echo -e "$msg" >>"$INSTALL_LOG"
}

log() { _log "${GREEN}[install]${NC} $*"; }
# warn:降级自error——安装过程中某些步骤失败不影响整体流程（如可选组件缺失），属于预期内可容忍的异常
warn() { _log "${YELLOW}[warn]${NC} $*"; }
error() { _log "${RED}[error]${NC} $*" >&2; }
info() { _log "${CYAN}[info]${NC} $*"; }

separator() { _log "============================================================"; }

require_sudo() {
	if ! command -v sudo &>/dev/null; then
		error "sudo command not found. Please install sudo or run as root."
		exit 1
	fi
	if ! sudo -n true 2>/dev/null; then
		log "This step requires sudo privileges. You may be prompted for password."
	fi
}

# ---- Error Report ----
generate_fail_report() {
	local exit_code=$?
	# Only generate report on non-zero exit
	[[ $exit_code -eq 0 ]] && return 0

	local report_ts
	report_ts=$(date '+%Y%m%d-%H%M%S')
	local report_file="${LOG_DIR}/install-fail-${report_ts}.md"

	mkdir -p "${LOG_DIR}"

	# Atomic write: use tempfile then mv (Rule 11)
	local tmp_file
	tmp_file=$(mktemp "${LOG_DIR}/install-fail-${report_ts}.tmp")

	local report_content
	report_content=$(
		cat <<REPORT_EOF
# ai-mirror Install Failure Report

- **Date**: $(date '+%Y-%m-%d %H:%M:%S')
- **Exit Code**: ${exit_code}
- **Failed Phase**: ${CURRENT_PHASE}
- **Error Message**: ${ERROR_MSG:-unknown (check install.log for details)}

## System Information

| Item | Value |
|------|-------|
| Hostname | $(hostname 2>/dev/null || echo 'N/A') |
| OS | $(cat /etc/os-release 2>/dev/null | grep '^PRETTY_NAME=' | cut -d'"' -f2 || uname -s) |
| Kernel | $(uname -r 2>/dev/null || echo 'N/A') |
| Architecture | $(uname -m 2>/dev/null || echo 'N/A') |
| CPU Cores | $(nproc 2>/dev/null || echo 'N/A') |
| Memory | $(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo 'N/A') |
| Disk (root) | $(df -h / 2>/dev/null | awk 'NR==2{print $2" total, "$3" used, "$4" available ("$5" used)"}' || echo 'N/A') |
| g++ | $(g++ -dumpversion 2>/dev/null || echo 'not found') |
| cmake | $(cmake --version 2>/dev/null | head -1 || echo 'not found') |
| make | $(make --version 2>/dev/null | head -1 || echo 'not found') |
| git | $(git --version 2>/dev/null || echo 'not found') |
| ssh | $(ssh -V 2>&1 || echo 'not found') |
| sudo | $(sudo --version 2>/dev/null | head -1 || echo 'not found') |

## Git Information

| Item | Value |
|------|-------|
| Remote URL | $(git -C "${SCRIPT_DIR}" remote get-url origin 2>/dev/null || echo 'no remote configured') |
| Branch | $(git -C "${SCRIPT_DIR}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'N/A') |
| Commit | $(git -C "${SCRIPT_DIR}" rev-parse --short HEAD 2>/dev/null || echo 'N/A') |
| Commit Date | $(git -C "${SCRIPT_DIR}" log -1 --format='%ci' 2>/dev/null || echo 'N/A') |
| Commit Message | $(git -C "${SCRIPT_DIR}" log -1 --format='%s' 2>/dev/null || echo 'N/A') |
| Dirty | $(if git -C "${SCRIPT_DIR}" diff --quiet 2>/dev/null; then echo 'no'; else echo 'yes'; fi) |

## Build Configuration

| Item | Value |
|------|-------|
| PREFIX | ${PREFIX} |
| CONFIG_DIR | ${CONFIG_DIR} |
| DATA_DIR | ${DATA_DIR} |
| BUILD_DIR | ${BUILD_DIR} |
| Build Type | Release |

## Install Log (last 50 lines)

\`\`\`
$(tail -50 "${INSTALL_LOG}" 2>/dev/null || echo 'install.log not available')
\`\`\`

## Actions

1. Review the error message and failed phase above
2. Check install.log for full details: \`${INSTALL_LOG}\`
3. Fix the issue and re-run: \`bash install.sh\`
REPORT_EOF
	)

	echo -e "$report_content" >"$tmp_file"
	chmod 0644 "$tmp_file"
	mv "$tmp_file" "$report_file"

	echo "" >&2
	echo -e "${RED}[install] INSTALL FAILED${NC}" >&2
	echo -e "${RED}[install] Failure report saved to: ${report_file}${NC}" >&2
	echo -e "${RED}[install] Please review and fix the issue, then re-run install.sh${NC}" >&2

	return $exit_code
}

trap generate_fail_report EXIT

# ---- Phase: Setup ----
phase_setup() {
	CURRENT_PHASE="setup"
	mkdir -p "${LOG_DIR}"
	mkdir -p "${BUILD_DIR}"
	LOG_DIR_READY=1

	: >"$INSTALL_LOG"

	separator
	info "ai-mirror installer v0.1.0"
	info "Log file: ${INSTALL_LOG}"
	separator

	log "Install started at $(date)"
	log "PREFIX=${PREFIX}"
	log "CONFIG_DIR=${CONFIG_DIR}"
	log "DATA_DIR=${DATA_DIR}"
}

# ---- Phase: Install system deps ----
phase_system_deps() {
	CURRENT_PHASE="system_deps"
	log "Checking system dependencies..."

	local -A pkg_cmd=(
		[cmake]=cmake
		[g++]=g++
		[make]=make
		[git]=git
		[openssh - server]=sshd
		[sudo]=sudo
	)
	local missing=()

	for pkg in "${!pkg_cmd[@]}"; do
		if ! command -v "${pkg_cmd[$pkg]}" &>/dev/null; then
			missing+=("$pkg")
		fi
	done

	if [[ ${#missing[@]} -gt 0 ]]; then
		require_sudo
		log "Installing missing packages: ${missing[*]}"
		if ! sudo apt-get update -qq 2>&1 | tee -a "$INSTALL_LOG"; then
			ERROR_MSG="apt-get update failed"
			return 1
		fi
		if ! sudo apt-get install -y -qq "${missing[@]}" 2>&1 | tee -a "$INSTALL_LOG"; then
			ERROR_MSG="apt-get install failed for: ${missing[*]}"
			return 1
		fi
	fi

	local gxx_version
	gxx_version=$(g++ -dumpversion 2>/dev/null || echo "unknown")
	local cmake_version
	cmake_version=$(cmake --version 2>/dev/null | head -1 || echo "unknown")

	log "  g++:    ${gxx_version}"
	log "  cmake:  ${cmake_version}"
	log "  git:    $(git --version 2>/dev/null || echo 'unknown')"

	if [[ "$(ssh -V 2>&1)" ]]; then
		log "  ssh:    $(ssh -V 2>&1)"
	fi

	log "System dependencies OK"
}

# ---- Phase: Build ----
phase_build() {
	CURRENT_PHASE="build"
	log "Building ai-mirror (C++20)..."

	mkdir -p "${BUILD_DIR}"

	local need_configure=false
	if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
		need_configure=true
		log "  No CMake cache found, running full configure..."
	elif [[ ! -f "${BUILD_DIR}/Makefile" ]]; then
		need_configure=true
		log "  CMake cache exists but Makefile missing, reconfiguring..."
	elif [[ "${SCRIPT_DIR}/CMakeLists.txt" -nt "${BUILD_DIR}/CMakeCache.txt" ]]; then
		need_configure=true
		log "  CMakeLists.txt changed, reconfiguring..."
	fi

	if $need_configure; then
		log "  Running cmake..."
		if ! cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
			-DCMAKE_CXX_COMPILER=g++ \
			-DCMAKE_BUILD_TYPE=Release \
			2>&1 | tee -a "$INSTALL_LOG"; then
			ERROR_MSG="CMake configure failed"
			return 1
		fi
	else
		log "  CMake cache exists, skipping configure..."
	fi

	log "  Compiling..."
	if ! cmake --build "${BUILD_DIR}" -j"$(nproc)" 2>&1 | tee -a "$INSTALL_LOG"; then
		ERROR_MSG="Build compilation failed"
		return 1
	fi

	log "Build successful"
}

# ---- Phase: Verify ----
phase_verify() {
	CURRENT_PHASE="verify"
	log "Verifying binary..."

	local bin="${BUILD_DIR}/bin/${BIN_NAME}"

	if [[ ! -x "$bin" ]]; then
		ERROR_MSG="Binary not found or not executable: ${bin}"
		return 1
	fi

	log "  ${BIN_NAME} ($(stat -c%s "$bin") bytes)"

	log "  Testing --help output..."
	"$bin" --help >/dev/null 2>&1 || {
		warn "  ${BIN_NAME} --help returned non-zero (may need root for full operation)"
	}

	log "Binary verified OK"
}

# ---- Phase: Install ----
phase_install() {
	CURRENT_PHASE="install"
	require_sudo
	log "Installing binary to ${PREFIX}/bin/..."

	if ! sudo install -d "${PREFIX}/bin"; then
		ERROR_MSG="Failed to create directory: ${PREFIX}/bin"
		return 1
	fi
	if ! sudo install -m 0755 "${BUILD_DIR}/bin/${BIN_NAME}" "${PREFIX}/bin/${REAL_BIN_NAME}"; then
		ERROR_MSG="Failed to install binary to ${PREFIX}/bin/${REAL_BIN_NAME}"
		return 1
	fi

	log "  ${PREFIX}/bin/${REAL_BIN_NAME}  ($(sudo stat -c%s "${PREFIX}/bin/${REAL_BIN_NAME}") bytes)"

	# Install bash profile function (only if content changed)
	log "Checking bash profile function..."
	local profile_src="${SCRIPT_DIR}/profile/am.sh"
	if [[ -f "$profile_src" ]]; then
		sudo install -d /etc/profile.d

		# Build expected content with search paths updated
		# The profile now searches multiple paths, ensure PREFIX/bin is first non-empty entry
		local expected_content
		expected_content=$(sed -E '/^_AM_SEARCH_PATHS=\(/,/\)/c\_AM_SEARCH_PATHS=("'"${PREFIX}/bin/${REAL_BIN_NAME}"'" "${AI_MIRROR_BIN:-}" "${HOME:-}/.local/bin/ai-mirror-bin")' "$profile_src")

		# Compare with installed version
		local needs_update=false
		if sudo test ! -f /etc/profile.d/am.sh; then
			needs_update=true
		else
			local installed_content
			installed_content=$(sudo cat /etc/profile.d/am.sh)
			if [[ "$expected_content" != "$installed_content" ]]; then
				needs_update=true
			fi
		fi

		if $needs_update; then
			echo "$expected_content" | sudo tee /etc/profile.d/am.sh >/dev/null
			sudo chmod 0644 /etc/profile.d/am.sh
			log "  /etc/profile.d/am.sh (updated)"
		else
			log "  /etc/profile.d/am.sh (unchanged, skipped)"
		fi
	else
		warn "  profile/am.sh not found, skipping profile function install"
	fi

	# Install bash completion
	log "Installing bash completion..."
	local completion_src="${SCRIPT_DIR}/completions/am-completion.bash"
	if [[ -f "$completion_src" ]]; then
		sudo install -d /etc/bash_completion.d
		sudo install -m 0644 "$completion_src" /etc/bash_completion.d/am
		log "  /etc/bash_completion.d/am"
	else
		warn "  completions/am-completion.bash not found, skipping bash completion install"
	fi

	log "Setting up data directory ${DATA_DIR}/..."
	sudo install -d "${DATA_DIR}"
	sudo chmod 0755 "${DATA_DIR}"

	log "Setting up config directory ${CONFIG_DIR}/..."
	sudo install -d "${CONFIG_DIR}"

	sudo install -d "${CONFIG_DIR}/sudoers.d"
	local sudoers_file="${CONFIG_DIR}/sudoers.d/ai-mirror"
	sudo tee "$sudoers_file" >/dev/null <<SUDOERS
# ai-mirror sudo rules
# Allows members of the ai-mirror group to run ai-mirror commands as root
#
# Security: Defense in depth
# - No wildcards in commands: exact binary path required
# - Subcommands restricted: only listed subcommands are allowed
# - "" suffix allows any arguments; validation enforced by binary
# - Binary validates all paths are under caller's home directory
# - Binary uses O_NOFOLLOW, fs::canonical, and boundary checks
# - Sudoers is the outer gate; binary is the inner validator
#
# Note: sudoers path patterns (e.g., /home/*) are limited glob syntax
# and cannot enforce username prefix validation. The binary performs
# strict path validation including:
# - Path must be under /home/<prefix>_username
# - No .. traversal
# - No symlink tricks (O_NOFOLLOW, canonical)
# - Parent directory must exist for new file creation
# The "" suffix allows any arguments after the subcommand.
# Argument validation is enforced by the binary itself.
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} create ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mkdir ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} touch ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} cp ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mv ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} cd ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} rm ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} force-destroy ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} health ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} list ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} config ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} status ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} update ""
SUDOERS
	sudo chmod 0440 "$sudoers_file"
	sudo chown root:root "$sudoers_file"

	if ! sudo getent group ai-mirror &>/dev/null; then
		sudo groupadd --system ai-mirror 2>/dev/null || true
		log "  Created system group: ai-mirror"
	fi

	log "Install phase complete"
}

# ---- Phase: Summary ----
phase_summary() {
	separator
	log "INSTALL COMPLETE"
	separator
	log ""
	log "Installed:"
	log "  /etc/profile.d/am.sh       (bash function, sourced on login)"
	log "  /etc/bash_completion.d/am  (bash completion, sourced on login)"
	log "  ${PREFIX}/bin/${REAL_BIN_NAME}  (actual binary)"
	log ""
	log "Data directory:"
	log "  ${DATA_DIR}/"
	log ""
	log "Sudoers:"
	log "  ${CONFIG_DIR}/sudoers.d/ai-mirror"
	log ""
	log "Quick start:"
	log "  1. Add user to group:     sudo usermod -aG ai-mirror \$USER"
	log "  2. (new login) source the profile function: source /etc/profile.d/am.sh"
	log "  3. Create project user:   am create /path/to/project"
	log "  4. Change directory:      am cd /path/to/project   (changes current shell)"
	log "  5. Grant write access:    am mkdir /path/to/dir <ai-user>"
	log "  6. List users:            am list"
	log "  7. Remove project:        am rm /path/to/project"
	log ""
	log "Uninstall:"
	log "  bash ${SCRIPT_DIR}/install.sh --clean"
	log ""
	log "Full log: ${INSTALL_LOG}"
	separator
}

# ---- Phase: Clean ----
phase_clean() {
	CURRENT_PHASE="clean"
	require_sudo
	log "Removing installed files..."

	if sudo test -f "${PREFIX}/bin/${REAL_BIN_NAME}"; then
		if ! sudo rm -f "${PREFIX}/bin/${REAL_BIN_NAME}"; then
			ERROR_MSG="Failed to remove ${PREFIX}/bin/${REAL_BIN_NAME}"
			return 1
		fi
		log "  Removed ${PREFIX}/bin/${REAL_BIN_NAME}"
	fi

	if sudo test -f /etc/profile.d/am.sh; then
		if ! sudo rm -f /etc/profile.d/am.sh; then
			ERROR_MSG="Failed to remove /etc/profile.d/am.sh"
			return 1
		fi
		log "  Removed /etc/profile.d/am.sh"
	fi

	if sudo test -f /etc/bash_completion.d/am; then
		if ! sudo rm -f /etc/bash_completion.d/am; then
			ERROR_MSG="Failed to remove /etc/bash_completion.d/am"
			return 1
		fi
		log "  Removed /etc/bash_completion.d/am"
	fi

	if sudo test -f "${CONFIG_DIR}/sudoers.d/ai-mirror"; then
		if ! sudo rm -f "${CONFIG_DIR}/sudoers.d/ai-mirror"; then
			ERROR_MSG="Failed to remove sudoers rule"
			return 1
		fi
		log "  Removed sudoers rule"
	fi

	if sudo test -d "${CONFIG_DIR}"; then
		if ! sudo rm -rf "${CONFIG_DIR}"; then
			ERROR_MSG="Failed to remove config dir ${CONFIG_DIR}/"
			return 1
		fi
		log "  Removed config dir ${CONFIG_DIR}/"
	fi

	if sudo test -d "${DATA_DIR}"; then
		if ! sudo rmdir "${DATA_DIR}" 2>/dev/null; then
			warn "  Data dir ${DATA_DIR}/ not empty, skipping removal"
		else
			log "  Removed data dir ${DATA_DIR}/"
		fi
	fi

	log "Uninstall complete"
}

# ---- Main ----
main() {
	local action="${1:-install}"

	case "$action" in
	--build)
		phase_setup || return $?
		phase_system_deps || return $?
		phase_build || return $?
		phase_verify || return $?
		log "Build-only complete. Binary in ${BUILD_DIR}/bin/"
		;;
	--clean)
		CURRENT_PHASE="clean"
		phase_setup || return $?
		phase_clean || return $?
		;;
	install | --install)
		phase_setup || return $?
		phase_system_deps || return $?
		phase_build || return $?
		phase_verify || return $?
		phase_install || return $?
		phase_summary || return $?
		;;
	--help | -h)
		echo "Usage: bash $0 [--build|--clean|--help]"
		echo ""
		echo "  (default)   Build and install (sudo used only when needed)"
		echo "  --build     Build and verify only, no sudo needed"
		echo "  --clean     Remove all installed files (sudo needed)"
		echo "  --help      Show this help"
		;;
	*)
		CURRENT_PHASE="main"
		ERROR_MSG="Unknown action: $action"
		error "Unknown action: $action"
		echo "Usage: bash $0 [--build|--clean|--help]"
		return 1
		;;
	esac
}

main "$@"

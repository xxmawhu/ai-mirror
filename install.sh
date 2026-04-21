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
WRAPPER_NAME="am"

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ---- Helpers ----
_log() {
	echo -e "$1"
	[[ -n "${LOG_DIR_READY:-}" ]] && echo -e "$1" >>"$INSTALL_LOG"
}

log() { _log "${GREEN}[install]${NC} $*"; }
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

# ---- Phase: Setup ----
phase_setup() {
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
		sudo apt-get update -qq
		sudo apt-get install -y -qq "${missing[@]}" 2>&1 | tee -a "$INSTALL_LOG"
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
	log "Building ai-mirror (C++20)..."

	mkdir -p "${BUILD_DIR}"

	local need_configure=false
	if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
		need_configure=true
		log "  No CMake cache found, running full configure..."
	elif [[ "${SCRIPT_DIR}/CMakeLists.txt" -nt "${BUILD_DIR}/CMakeCache.txt" ]]; then
		need_configure=true
		log "  CMakeLists.txt changed, reconfiguring..."
	fi

	if $need_configure; then
		log "  Running cmake..."
		cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
			-DCMAKE_CXX_COMPILER=g++ \
			-DCMAKE_BUILD_TYPE=Release \
			2>&1 | tee -a "$INSTALL_LOG"

		if [[ ${PIPESTATUS[0]} -ne 0 ]]; then
			error "CMake configure failed. Check ${INSTALL_LOG}"
			exit 1
		fi
	else
		log "  CMake cache exists, skipping configure..."
	fi

	log "  Compiling..."
	cmake --build "${BUILD_DIR}" -j"$(nproc)" 2>&1 | tee -a "$INSTALL_LOG"

	if [[ ${PIPESTATUS[0]} -ne 0 ]]; then
		error "Build failed. Check ${INSTALL_LOG}"
		exit 1
	fi

	log "Build successful"
}

# ---- Phase: Verify ----
phase_verify() {
	log "Verifying binary..."

	local bin="${BUILD_DIR}/bin/${BIN_NAME}"

	if [[ ! -x "$bin" ]]; then
		error "Binary not found: ${bin}"
		exit 1
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
	require_sudo
	log "Installing binary to ${PREFIX}/bin/..."

	sudo install -d "${PREFIX}/bin"
	sudo install -m 0755 "${BUILD_DIR}/bin/${BIN_NAME}" "${PREFIX}/bin/${REAL_BIN_NAME}"

	log "  ${PREFIX}/bin/${REAL_BIN_NAME}  ($(sudo stat -c%s "${PREFIX}/bin/${REAL_BIN_NAME}") bytes)"

	log "Installing 'am' wrapper to ${PREFIX}/bin/..."
	local wrapper_src="${SCRIPT_DIR}/docker/am-wrapper.sh"
	if [[ ! -f "$wrapper_src" ]]; then
		wrapper_src="${SCRIPT_DIR}/am-wrapper.sh"
	fi
	if [[ -f "$wrapper_src" ]]; then
		sudo install -m 0755 "$wrapper_src" "${PREFIX}/bin/${WRAPPER_NAME}"
		sudo sed -i "s|/usr/local/bin/ai-mirror-bin|${PREFIX}/bin/${REAL_BIN_NAME}|" "${PREFIX}/bin/${WRAPPER_NAME}"
		log "  ${PREFIX}/bin/${WRAPPER_NAME} (wrapper)"
	else
		sudo tee "${PREFIX}/bin/${WRAPPER_NAME}" >/dev/null <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
AM_BIN="${PREFIX}/bin/${REAL_BIN_NAME}"
if [ ! -x "\$AM_BIN" ]; then
    echo "error: \$AM_BIN not found" >&2
    exit 1
fi

# Security: validate parsed output before use.  The wrapper parses output from
# the C++ binary (run via sudo) and passes it to ssh/cd.  A compromised or
# buggy binary could emit injected values.  These guards ensure:
# - path must start with \$HOME or /home/ (prevent arbitrary directory access)
# - user must start with ai- (prevent SSH to arbitrary users)
if [ "\${1:-}" = "cd" ]; then
    shift
    output=\$(sudo --preserve-env=HOME "\$AM_BIN" cd "\$@")
    action=\$(echo "\$output" | grep '^action=' | cut -d= -f2)
    if [ "\$action" = "ssh" ]; then
        user=\$(echo "\$output" | grep '^user=' | cut -d= -f2)
        path=\$(echo "\$output" | grep '^path=' | cut -d= -f2)
        if ! echo "\$path" | grep -qE "^(\$HOME/|/home/)"; then
            echo "error: invalid path from am cd: '\$path'" >&2
            exit 1
        fi
        if ! echo "\$user" | grep -qE '^ai-'; then
            echo "error: invalid user from am cd: '\$user' (must start with ai-)" >&2
            exit 1
        fi
        exec ssh "\${user}@localhost" -t "cd '\${path}'"
    elif [ "\$action" = "cd" ]; then
        path=\$(echo "\$output" | grep '^path=' | cut -d= -f2)
        if ! echo "\$path" | grep -qE "^(\$HOME/|/home/)"; then
            echo "error: invalid path from am cd: '\$path'" >&2
            exit 1
        fi
        echo "cd '\${path}'"
    else
        echo "error: unknown action from am cd" >&2
        exit 1
    fi
    exit 0
fi

if [ "\$(id -u)" -eq 0 ]; then
    exec "\$AM_BIN" "\$@"
fi
exec sudo --preserve-env=HOME "\$AM_BIN" "\$@"
WRAPPER
		sudo chmod 0755 "${PREFIX}/bin/${WRAPPER_NAME}"
		log "  ${PREFIX}/bin/${WRAPPER_NAME} (wrapper, inline)"
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
# - No argument restrictions in sudoers: validation enforced by binary
# - Binary validates all paths start with /home/<prefix>_
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
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} create
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mkdir
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} touch
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} cp
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mv
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} cd
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} rm
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} force-destroy
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} health
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} list
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} config
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} status
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
	log "  ${PREFIX}/bin/${WRAPPER_NAME}       (wrapper, auto-invokes sudo)"
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
	log "  2. Create project user:   am create /path/to/project"
	log "  3. Grant write access:    am mkdir /path/to/dir <ai-user>"
	log "  4. Create file:           am touch /path/to/file <ai-user>"
	log "  5. List users:            am list"
	log "  6. Remove project:        am rm /path/to/project"
	log ""
	log "Uninstall:"
	log "  bash ${SCRIPT_DIR}/install.sh --clean"
	log ""
	log "Full log: ${INSTALL_LOG}"
	separator
}

# ---- Phase: Clean ----
phase_clean() {
	require_sudo
	log "Removing installed files..."

	if sudo test -f "${PREFIX}/bin/${REAL_BIN_NAME}"; then
		sudo rm -f "${PREFIX}/bin/${REAL_BIN_NAME}"
		log "  Removed ${PREFIX}/bin/${REAL_BIN_NAME}"
	fi

	if sudo test -f "${PREFIX}/bin/${WRAPPER_NAME}"; then
		sudo rm -f "${PREFIX}/bin/${WRAPPER_NAME}"
		log "  Removed ${PREFIX}/bin/${WRAPPER_NAME}"
	fi

	if sudo test -f "${CONFIG_DIR}/sudoers.d/ai-mirror"; then
		sudo rm -f "${CONFIG_DIR}/sudoers.d/ai-mirror"
		log "  Removed sudoers rule"
	fi

	if sudo test -d "${CONFIG_DIR}"; then
		sudo rm -rf "${CONFIG_DIR}"
		log "  Removed config dir ${CONFIG_DIR}/"
	fi

	if sudo test -d "${DATA_DIR}"; then
		sudo rmdir "${DATA_DIR}" 2>/dev/null && log "  Removed data dir ${DATA_DIR}/" || true
	fi

	log "Uninstall complete"
}

# ---- Main ----
main() {
	local action="${1:-install}"

	case "$action" in
	--build)
		phase_setup
		phase_system_deps
		phase_build
		phase_verify
		log "Build-only complete. Binary in ${BUILD_DIR}/bin/"
		;;
	--clean)
		phase_setup
		phase_clean
		;;
	install | --install)
		phase_setup
		phase_system_deps
		phase_build
		phase_verify
		phase_install
		phase_summary
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
		error "Unknown action: $action"
		echo "Usage: bash $0 [--build|--clean|--help]"
		exit 1
		;;
	esac
}

main "$@"

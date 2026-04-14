#!/usr/bin/env bash
#
# ai-mirror installer
# Builds from source and installs ai-mirror CLI.
# Includes systemd health-check daemon setup.
#
# Usage:
#   chmod +x install.sh
#   sudo ./install.sh            # full install (build + deploy + systemd)
#   sudo ./install.sh --build    # build only, do not install
#   sudo ./install.sh --clean    # remove installed files
#
set -euo pipefail

# ---- Configuration ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-release"
LOG_DIR="${SCRIPT_DIR}/log"
INSTALL_LOG="${LOG_DIR}/install.log"

PREFIX="${AI_MIRROR_PREFIX:-/usr/local}"
SYSTEMD_DIR="${AI_MIRROR_SYSTEMD_DIR:-/etc/systemd/system}"
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
log() { echo -e "${GREEN}[install]${NC} $*" | tee -a "$INSTALL_LOG"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*" | tee -a "$INSTALL_LOG"; }
error() { echo -e "${RED}[error]${NC} $*" | tee -a "$INSTALL_LOG" >&2; }
info() { echo -e "${CYAN}[info]${NC} $*" | tee -a "$INSTALL_LOG"; }

separator() {
	echo "============================================================" | tee -a "$INSTALL_LOG"
}

check_root() {
	if [[ $EUID -ne 0 ]]; then
		error "This script must be run as root (sudo)."
		exit 1
	fi
}

# ---- Phase: Setup ----
phase_setup() {
	mkdir -p "${LOG_DIR}"
	mkdir -p "${BUILD_DIR}"

	: >"$INSTALL_LOG"

	separator
	info "ai-mirror installer v0.1.0"
	info "Log file: ${INSTALL_LOG}"
	separator

	log "Install started at $(date)"
	log "PREFIX=${PREFIX}"
	log "SYSTEMD_DIR=${SYSTEMD_DIR}"
	log "CONFIG_DIR=${CONFIG_DIR}"
	log "DATA_DIR=${DATA_DIR}"
}

# ---- Phase: Install system deps ----
phase_system_deps() {
	log "Checking system dependencies..."

	local deps=(cmake g++ make git openssh-server sudo)
	local missing=()

	for dep in "${deps[@]}"; do
		if ! command -v "$dep" &>/dev/null; then
			missing+=("$dep")
		fi
	done

	if [[ ${#missing[@]} -gt 0 ]]; then
		log "Installing missing packages: ${missing[*]}"
		apt-get update -qq
		apt-get install -y -qq "${missing[@]}" 2>&1 | tee -a "$INSTALL_LOG"
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
	log "Installing binary to ${PREFIX}/bin/..."

	install -d "${PREFIX}/bin"
	install -m 0755 "${BUILD_DIR}/bin/${BIN_NAME}" "${PREFIX}/bin/${REAL_BIN_NAME}"

	log "  ${PREFIX}/bin/${REAL_BIN_NAME}  ($(stat -c%s "${PREFIX}/bin/${REAL_BIN_NAME}") bytes)"

	log "Installing 'am' wrapper to ${PREFIX}/bin/..."
	local wrapper_src="${SCRIPT_DIR}/docker/am-wrapper.sh"
	if [[ ! -f "$wrapper_src" ]]; then
		wrapper_src="${SCRIPT_DIR}/am-wrapper.sh"
	fi
	if [[ -f "$wrapper_src" ]]; then
		install -m 0755 "$wrapper_src" "${PREFIX}/bin/${WRAPPER_NAME}"
		sed -i "s|/usr/local/bin/ai-mirror-bin|${PREFIX}/bin/${REAL_BIN_NAME}|" "${PREFIX}/bin/${WRAPPER_NAME}"
		log "  ${PREFIX}/bin/${WRAPPER_NAME} (wrapper)"
	else
		cat >"${PREFIX}/bin/${WRAPPER_NAME}" <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
AM_BIN="${PREFIX}/bin/${REAL_BIN_NAME}"
if [ ! -x "\$AM_BIN" ]; then
    echo "error: \$AM_BIN not found" >&2
    exit 1
fi
if [ "\$(id -u)" -eq 0 ]; then
    exec "\$AM_BIN" "\$@"
fi
exec sudo "\$AM_BIN" "\$@"
WRAPPER
		chmod 0755 "${PREFIX}/bin/${WRAPPER_NAME}"
		log "  ${PREFIX}/bin/${WRAPPER_NAME} (wrapper, inline)"
	fi

	log "Setting up data directory ${DATA_DIR}/..."
	install -d "${DATA_DIR}"
	chmod 0755 "${DATA_DIR}"

	log "Installing default config to ${CONFIG_DIR}/..."
	install -d "${CONFIG_DIR}"

	local config_file="${CONFIG_DIR}/ai-mirror.toml"
	if [[ ! -f "$config_file" ]]; then
		cat >"$config_file" <<'TOML'
# ai-mirror configuration
# See: ai-mirror config

[user]
prefix = "i"

[mount]
paths = [
    "~/.bashrc",
    "~/.config",
]

[ssh]
key_type = "ed25519"
key_path = "~/.ssh/ai-mirror"

[log]
auth_log = "/var/log/auth.log"
level = "info"
TOML
		chmod 0644 "$config_file"
		log "  ${config_file} (created)"
	else
		log "  Preserving existing config: ${config_file}"
	fi

	install -d "${CONFIG_DIR}/sudoers.d"
	local sudoers_file="${CONFIG_DIR}/sudoers.d/ai-mirror"
	cat >"$sudoers_file" <<SUDOERS
# ai-mirror sudo rules
# Allows members of the ai-mirror group to run ai-mirror commands as root
# The 'am' wrapper auto-invokes sudo, so users type: am create /path
# Security: uses "" for exact arg count; path validation is enforced by the binary
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} create "",*
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} mkdir "" ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} cd "",*
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} rm "",*
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} force-destroy "",*
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} health
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} list
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} config
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${REAL_BIN_NAME} status
SUDOERS
	chmod 0440 "$sudoers_file"

	if ! getent group ai-mirror &>/dev/null; then
		groupadd --system ai-mirror 2>/dev/null || true
		log "  Created system group: ai-mirror"
	fi

	log "Install phase complete"
}

# ---- Phase: Systemd ----
has_systemd() {
	if [ -d /run/systemd/system ] 2>/dev/null; then
		return 0
	fi
	return 1
}

phase_systemd() {
	if ! has_systemd; then
		warn "systemd not running (PID 1 != systemd). Skipping systemd setup."
		warn "In container/WSL environments, health check runs via cron or manual invocation."
		return 0
	fi

	log "Setting up systemd health-check service..."

	local was_running=false
	if systemctl is-active --quiet ai-mirror-health.service 2>/dev/null; then
		was_running=true
		log "  Service is currently running, will restart after update"
	fi

	if $was_running; then
		systemctl stop ai-mirror-health.service 2>&1 | tee -a "$INSTALL_LOG" || true
		log "  Stopped existing service"
	fi

	cat >"${SYSTEMD_DIR}/ai-mirror-health.service" <<EOF
[Unit]
Description=ai-mirror Periodic Health Check
Documentation=man:ai-mirror(1)
After=local-fs.target

[Service]
Type=oneshot
	ExecStart=${PREFIX}/bin/${REAL_BIN_NAME} health
StandardOutput=journal
StandardError=journal
EOF

	chmod 0644 "${SYSTEMD_DIR}/ai-mirror-health.service"
	log "  ${SYSTEMD_DIR}/ai-mirror-health.service"

	cat >"${SYSTEMD_DIR}/ai-mirror-health.timer" <<EOF
[Unit]
Description=Run ai-mirror health check every 5 minutes

[Timer]
OnBootSec=1min
OnUnitActiveSec=5min
AccuracySec=30s

[Install]
WantedBy=timers.target
EOF

	chmod 0644 "${SYSTEMD_DIR}/ai-mirror-health.timer"
	log "  ${SYSTEMD_DIR}/ai-mirror-health.timer"

	systemctl daemon-reload 2>&1 | tee -a "$INSTALL_LOG"
	log "  systemd daemon-reloaded"

	systemctl enable ai-mirror-health.timer 2>&1 | tee -a "$INSTALL_LOG"
	log "  ai-mirror-health.timer enabled"

	if $was_running; then
		systemctl start ai-mirror-health.timer 2>&1 | tee -a "$INSTALL_LOG"
		log "  Timer restarted"
	else
		echo ""
		info "Health timer is enabled but NOT started yet."
		info "Start with: sudo systemctl start ai-mirror-health.timer"
	fi

	echo ""
	info "Check timer:  sudo systemctl list-timers ai-mirror-health.timer"
	info "Manual check: ${PREFIX}/bin/${WRAPPER_NAME} health"
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
	log "Configuration:"
	log "  ${CONFIG_DIR}/ai-mirror.toml"
	log ""
	log "Data directory:"
	log "  ${DATA_DIR}/"
	log ""
	log "Systemd timer:"
	log "  ${SYSTEMD_DIR}/ai-mirror-health.timer (every 5 min)"
	log ""
	log "Sudoers:"
	log "  ${CONFIG_DIR}/sudoers.d/ai-mirror"
	log ""
	log "Quick start:"
	log "  1. Add user to group:     sudo usermod -aG ai-mirror \$USER"
	log "  2. Create config symlink: ln -s ${CONFIG_DIR}/ai-mirror.toml ~/.ai-mirror.toml"
	log "  3. Create project user:   am create /path/to/project"
	log "  4. Grant write access:    am mkdir /path/to/dir <ai-user>"
	log "  5. List users:            am list"
	log "  6. Remove project:        am rm /path/to/project"
	log ""
	log "Uninstall:"
	log "  sudo ${SCRIPT_DIR}/install.sh --clean"
	log ""
	log "Full log: ${INSTALL_LOG}"
	separator
}

# ---- Phase: Clean ----
phase_clean() {
	log "Removing installed files..."

	if has_systemd; then
		if systemctl is-active --quiet ai-mirror-health.timer 2>/dev/null; then
			log "  Stopping timer..."
			systemctl stop ai-mirror-health.timer 2>&1 | tee -a "$INSTALL_LOG" || true
		fi
		if systemctl is-active --quiet ai-mirror-health.service 2>/dev/null; then
			log "  Stopping service..."
			systemctl stop ai-mirror-health.service 2>&1 | tee -a "$INSTALL_LOG" || true
		fi
		if systemctl is-enabled --quiet ai-mirror-health.timer 2>/dev/null; then
			log "  Disabling timer..."
			systemctl disable ai-mirror-health.timer 2>&1 | tee -a "$INSTALL_LOG" || true
		fi
	fi

	for f in ai-mirror-health.service ai-mirror-health.timer; do
		if [[ -f "${SYSTEMD_DIR}/${f}" ]]; then
			rm -f "${SYSTEMD_DIR}/${f}"
			log "  Removed ${SYSTEMD_DIR}/${f}"
		fi
	done

	if has_systemd; then
		systemctl daemon-reload 2>&1 | tee -a "$INSTALL_LOG" || true
	fi

	if [[ -f "${PREFIX}/bin/${REAL_BIN_NAME}" ]]; then
		rm -f "${PREFIX}/bin/${REAL_BIN_NAME}"
		log "  Removed ${PREFIX}/bin/${REAL_BIN_NAME}"
	fi

	if [[ -f "${PREFIX}/bin/${WRAPPER_NAME}" ]]; then
		rm -f "${PREFIX}/bin/${WRAPPER_NAME}"
		log "  Removed ${PREFIX}/bin/${WRAPPER_NAME}"
	fi

	if [[ -f "${CONFIG_DIR}/sudoers.d/ai-mirror" ]]; then
		rm -f "${CONFIG_DIR}/sudoers.d/ai-mirror"
		log "  Removed sudoers rule"
	fi

	if [[ -d "${CONFIG_DIR}" ]]; then
		if [[ -f "${CONFIG_DIR}/ai-mirror.toml" ]]; then
			local backup="/tmp/ai-mirror.toml.bak.$(date +%Y%m%d%H%M%S)"
			cp "${CONFIG_DIR}/ai-mirror.toml" "$backup" 2>/dev/null || true
			log "  Backed up config to ${backup}"
		fi
		rm -rf "${CONFIG_DIR}"
		log "  Removed config dir ${CONFIG_DIR}/"
	fi

	if [[ -d "${DATA_DIR}" ]]; then
		rmdir "${DATA_DIR}" 2>/dev/null && log "  Removed data dir ${DATA_DIR}/" || true
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
		check_root
		phase_setup
		phase_clean
		;;
	install | --install)
		check_root
		phase_setup
		phase_system_deps
		phase_build
		phase_verify
		phase_install
		phase_systemd
		phase_summary
		;;
	--help | -h)
		echo "Usage: $0 [--build|--clean|--help]"
		echo ""
		echo "  (default)   Build, install, and setup systemd timer"
		echo "  --build     Build and verify only, do not install"
		echo "  --clean     Remove all installed files and systemd services"
		echo "  --help      Show this help"
		;;
	*)
		error "Unknown action: $action"
		echo "Usage: $0 [--build|--clean|--help]"
		exit 1
		;;
	esac
}

main "$@"

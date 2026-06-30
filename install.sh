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
BIN_NAME="ai-mirror-bin"
WRAPPER_NAME="am"
MOUNT_WATCH_NAME="am-mount-watch"

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
# Screen + log (key status lines only)
_log() {
	local ts
	ts=$(date '+%Y-%m-%d %H:%M:%S')
	local msg="${ts} $1"
	echo -e "$msg"
	# [防御] Permission denied 时静默降级（log 文件被其他用户创建时），set -e 不阻断
	[[ -n "${LOG_DIR_READY:-}" ]] && echo -e "$msg" >>"$INSTALL_LOG" 2>/dev/null || true
}
# Log only (verbose steps — never shown on screen)
_log_file() {
	# [防御] Permission denied 时静默降级，set -e 不阻断
	[[ -n "${LOG_DIR_READY:-}" ]] && echo -e "$(date '+%Y-%m-%d %H:%M:%S') $1" >>"$INSTALL_LOG" 2>/dev/null || true
}

log() { _log "${GREEN}[install]${NC} $*"; }
# warn:降级自error——安装过程中某些步骤失败不影响整体流程（如可选组件缺失），属于预期内可容忍的异常
warn() { _log "${YELLOW}[warn]${NC} $*"; }
error() { _log "${RED}[error]${NC} $*" >&2; }
info() { _log "${CYAN}[info]${NC} $*"; }
# Key step status: the ONLY lines a user sees for routine progress
ok() { _log "${GREEN}[OK]${NC}   $*"; }
fail() { _log "${RED}[FAIL]${NC} $*"; }

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

	mkdir -p "${LOG_DIR}" 2>/dev/null || true

	# Atomic write: use tempfile then mv (Rule 11)
	local tmp_file
	tmp_file=$(mktemp "${LOG_DIR}/install-fail-XXXXXX.tmp" 2>/dev/null) || {
		echo "[install] INSTALL FAILED (exit=$exit_code)" >&2
		echo "[install] Cannot write failure report to ${LOG_DIR}/ (Permission denied)" >&2
		return $exit_code
	}

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
	mkdir -p "${LOG_DIR}" 2>/dev/null || true
	mkdir -p "${BUILD_DIR}" 2>/dev/null || true
	LOG_DIR_READY=1

	# [防御] 清空 log 文件可能因权限失败（文件被其他用户创建），静默降级
	: >"$INSTALL_LOG" 2>/dev/null || true

	info "ai-mirror installer v0.1.0 (log: ${INSTALL_LOG})"
	_log_file "Install started at $(date)"
	_log_file "PREFIX=${PREFIX} CONFIG_DIR=${CONFIG_DIR} DATA_DIR=${DATA_DIR}"
}

# ---- Phase: Install system deps ----
phase_system_deps() {
	CURRENT_PHASE="system_deps"

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
		info "Installing missing packages: ${missing[*]}"
		if ! sudo apt-get update -qq 2>&1 | sudo tee -a "$INSTALL_LOG" >/dev/null; then
			ERROR_MSG="apt-get update failed"
			fail "依赖安装失败 (apt-get update)"
			return 1
		fi
		if ! sudo apt-get install -y -qq "${missing[@]}" 2>&1 | sudo tee -a "$INSTALL_LOG" >/dev/null; then
			ERROR_MSG="apt-get install failed for: ${missing[*]}"
			fail "依赖安装失败: ${missing[*]}"
			return 1
		fi
	fi

	# Versions go to log only
	_log_file "g++=$(g++ -dumpversion 2>/dev/null || echo unknown) cmake=$(cmake --version 2>/dev/null | head -1 || echo unknown) git=$(git --version 2>/dev/null || echo unknown)"

	ok "系统依赖"
}

# ---- Phase: Build ----
phase_build() {
	CURRENT_PHASE="build"
	info "构建中 (C++20)..."

	# [防御] build dir 创建失败时静默降级（Permission denied）
	mkdir -p "${BUILD_DIR}" 2>/dev/null || true

	# Check CMakeLists.txt content hash for reliable change detection
	# (timestamp -nt fails when git pull or cp -p preserves timestamps)
	local cmake_hash_file="${BUILD_DIR}/.cmake-lists-hash"
	local current_hash prev_hash=""
	current_hash=$(md5sum "${SCRIPT_DIR}/CMakeLists.txt" 2>/dev/null | cut -d' ' -f1 || echo "no-hash")
	[[ -f "$cmake_hash_file" ]] && prev_hash=$(cat "$cmake_hash_file" 2>/dev/null || echo "")

	local need_configure=false
	if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
		need_configure=true
	elif [[ ! -f "${BUILD_DIR}/Makefile" ]]; then
		need_configure=true
	elif [[ "$current_hash" != "$prev_hash" ]]; then
		_log_file "CMakeLists.txt changed (hash: ${prev_hash:-none} -> $current_hash)"
		need_configure=true
	else
		# cache_mtime intentionally removed: we use find -newer below which is
		# more reliable (handles mtime comparison correctly across filesystems)
		local newer_src
		newer_src=$(find "${SCRIPT_DIR}/src" "${SCRIPT_DIR}/include" \
			-type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
			-newer "${BUILD_DIR}/CMakeCache.txt" -print -quit 2>/dev/null)
		if [[ -n "$newer_src" ]]; then
			need_configure=true
		fi
	fi
	_log_file "need_configure=$need_configure"

	if $need_configure; then
		if ! cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
			-DCMAKE_CXX_COMPILER=g++ \
			-DCMAKE_BUILD_TYPE=Release \
			>>"$INSTALL_LOG" 2>&1; then
			ERROR_MSG="CMake configure failed"
			fail "构建失败 (cmake 配置)"
			{
				echo ""
				echo "── cmake 配置错误 ──"
				grep -i -E "CMake (Error|Warning)|^CMake Error|error:" "$INSTALL_LOG" 2>/dev/null ||
					tail -15 "$INSTALL_LOG" 2>/dev/null
				echo "────────────────────"
			} >&2
			return 1
		fi
		# Save hash after successful configure (only if cmake succeeded)
		echo "$current_hash" >"$cmake_hash_file" 2>/dev/null || true
	fi

	if ! cmake --build "${BUILD_DIR}" -j"$(nproc)" >>"$INSTALL_LOG" 2>&1; then
		ERROR_MSG="Build compilation failed"
		fail "构建失败 (编译)"
		{
			echo ""
			echo "── 编译错误 ──"
			grep -i -E "error:|fatal error:|undefined reference|cannot find|CMake Error" "$INSTALL_LOG" 2>/dev/null |
				head -30 ||
				tail -15 "$INSTALL_LOG" 2>/dev/null
			echo "──────────────"
		} >&2
		return 1
	fi

	ok "构建"
}

# ---- Phase: Verify ----
phase_verify() {
	CURRENT_PHASE="verify"

	local bin="${BUILD_DIR}/bin/${BIN_NAME}"

	if [[ ! -x "$bin" ]]; then
		ERROR_MSG="Binary not found or not executable: ${bin}"
		fail "验证失败 (二进制缺失)"
		return 1
	fi

	_log_file "binary=$(stat -c%s "$bin") bytes"

	local help_output help_exit
	help_output=$("$bin" --help 2>&1) && help_exit=0 || help_exit=$?

	if [[ $help_exit -ne 0 ]]; then
		_log_file "${BIN_NAME} --help returned non-zero (exit=$help_exit): ${help_output}"
		# [log-review] warn:降级自error——--help 非零退出可能是终端兼容问题，不影响安装
		warn "验证: --help 返回非零 (exit=$help_exit，可能终端兼容问题)"
	else
		ok "验证"
	fi
}

# ---- Version Management ----
get_version() {
	# Extract version from CMakeLists.txt: project(ai-mirror VERSION <major>.<minor>)
	local version
	version=$(grep -oP 'project\([^)]*VERSION\s+\K[0-9]+\.[0-9]+' "${SCRIPT_DIR}/CMakeLists.txt" 2>/dev/null || echo "1.0")
	echo "$version"
}

cleanup_old_versions() {
	local bin_dir="$1"
	local name="$2"
	local current_version="$3"
	local max_keep=2 # Keep at most 2 old versions

	# Find all versioned files, sorted by modification time (newest first)
	local versions=()
	while IFS= read -r -d '' f; do
		versions+=("$f")
	done < <(find "${bin_dir}" -maxdepth 1 -type f -name "${name}.*" \
		-regextype posix-extended -regex ".*/${name}\.[0-9]+\.[0-9]+$" \
		-printf '%T@ %p\0' 2>/dev/null | sort -rn | cut -z -d' ' -f2-)

	local count
	count=${#versions[@]}
	if [[ $count -le $max_keep ]]; then
		return 0
	fi

	# Delete oldest versions beyond max_keep
	local i
	for ((i = max_keep; i < count; i++)); do
		local f="${versions[$i]}"
		if [[ "$f" != "${bin_dir}/${name}.${current_version}" ]]; then
			log "  Removing old version: $(basename "$f")"
			sudo rm -f "$f"
		fi
	done
}

# ---- Phase: Install ----
phase_install() {
	CURRENT_PHASE="install"
	require_sudo

	local VERSION
	VERSION=$(get_version)

	if ! sudo install -d "${PREFIX}/bin"; then
		ERROR_MSG="Failed to create directory: ${PREFIX}/bin"
		fail "安装失败 (创建 ${PREFIX}/bin)"
		return 1
	fi

	if ! sudo install -m 0755 "${BUILD_DIR}/bin/${WRAPPER_NAME}" "${PREFIX}/bin/${WRAPPER_NAME}"; then
		ERROR_MSG="Failed to install wrapper to ${PREFIX}/bin/${WRAPPER_NAME}"
		fail "安装失败 (${WRAPPER_NAME})"
		return 1
	fi

	local VERSIONED_BIN="${BIN_NAME}.${VERSION}"
	if ! sudo install -m 0755 "${BUILD_DIR}/bin/${BIN_NAME}" "${PREFIX}/bin/${VERSIONED_BIN}"; then
		ERROR_MSG="Failed to install versioned binary to ${PREFIX}/bin/${VERSIONED_BIN}"
		fail "安装失败 (${VERSIONED_BIN})"
		return 1
	fi

	# Create symlink: ai-mirror-bin -> ai-mirror-bin.<version> (relative path, atomic)
	cd "${PREFIX}/bin"
	if ! sudo ln -sfn "${VERSIONED_BIN}" "${BIN_NAME}"; then
		ERROR_MSG="Failed to create symlink ${BIN_NAME} -> ${VERSIONED_BIN}"
		fail "安装失败 (symlink)"
		cd "${SCRIPT_DIR}"
		return 1
	fi
	cd "${SCRIPT_DIR}"

	# Install am-mount-watch (standalone mount health checker for systemd)
	if [[ -f "${BUILD_DIR}/bin/${MOUNT_WATCH_NAME}" ]]; then
		if ! sudo install -m 0755 "${BUILD_DIR}/bin/${MOUNT_WATCH_NAME}" "${PREFIX}/bin/${MOUNT_WATCH_NAME}"; then
			ERROR_MSG="Failed to install ${MOUNT_WATCH_NAME}"
			fail "安装失败 (${MOUNT_WATCH_NAME})"
			return 1
		fi
		_log_file "installed: ${PREFIX}/bin/${MOUNT_WATCH_NAME}"

		# ---- 内联 systemd 配置（完整维护逻辑，不委托外部脚本） ----
		if command -v systemctl &>/dev/null; then
			log "配置 am-mount-watch systemd 服务（内联）..."
			_log_file "Setting up am-mount-watch systemd service (inline)"

			# 1) 创建 service 单元文件
			local svc_file="/etc/systemd/system/am-mount-watch.service"
			sudo tee "$svc_file" >/dev/null <<UNIT_EOF
[Unit]
Description=am-mount-watch — AI mirror mount health checker & auto-repair
Documentation=https://github.com/maxx/ai-mirror
After=local-fs.target

[Service]
Type=oneshot
ExecStart=${PREFIX}/bin/${MOUNT_WATCH_NAME}
User=root
Group=root
StandardOutput=journal
StandardError=journal
SyslogIdentifier=am-mount-watch

# Hardening
CapabilityBoundingSet=
PrivateTmp=yes
NoNewPrivileges=yes
ProtectSystem=full
ProtectHome=read-only
RestrictSUIDSGID=yes

[Install]
WantedBy=multi-user.target
UNIT_EOF
			local svc_rc=$?
			# [log-review] warn:降级自error——systemd service 单元写入失败是预期内异常（sudo tee 可能在非 systemd 系统或容器内失败），不影响主程序安装
			[[ $svc_rc -ne 0 ]] && {
				warn "创建 am-mount-watch.service 失败"
				ERROR_MSG="systemd service unit creation failed"
			}

			# 2) 创建 timer 单元文件
			local tmr_file="/etc/systemd/system/am-mount-watch.timer"
			sudo tee "$tmr_file" >/dev/null <<UNIT_EOF
[Unit]
Description=am-mount-watch timer (every 5 min)
Requires=am-mount-watch.service

[Timer]
OnCalendar=*:0/5
Persistent=true
RandomizedDelaySec=30

[Install]
WantedBy=timers.target
UNIT_EOF
			# [log-review] warn:降级自error——systemd timer 单元写入失败是预期内异常（可能 sudo 超时或 systemd 不支持），不影响主程序安装
			local tmr_rc=$?
			[[ $tmr_rc -ne 0 ]] && {
				warn "创建 am-mount-watch.timer 失败"
				ERROR_MSG="systemd timer unit creation failed"
			}

			# 3) 重载 daemon + 启用 timer + 启动 timer + 立即运行一次
			if [[ $svc_rc -eq 0 && $tmr_rc -eq 0 ]]; then
				sudo systemctl daemon-reload 2>/dev/null || true
				sudo systemctl enable am-mount-watch.timer 2>/dev/null || true
				sudo systemctl start am-mount-watch.timer 2>/dev/null || true
				_log_file "systemd: timer enabled and started"
				# 立即执行一次（oneshot，不等 5 分钟周期）
				sudo systemctl start am-mount-watch.service 2>/dev/null || true
				_log_file "systemd: immediate check triggered"
				ok "am-mount-watch systemd 服务已安装并启动"
			else
				# [log-review] warn:降级自error——systemd 单元文件写入失败后单元可能不完整，但不影响 ai-mirror 主程序功能
				warn "am-mount-watch systemd 单元文件创建异常，请检查 systemd 状态"
			fi
		else
			# [log-review] warn:降级自error——systemctl 不可用（容器/非 systemd 系统），跳过 systemd 配置不影响主程序安装
			warn "systemctl 不可用，跳过 systemd 配置"
		fi
	fi

	_log_file "installed: ${PREFIX}/bin/${WRAPPER_NAME}, ${PREFIX}/bin/${VERSIONED_BIN} -> ${BIN_NAME}"

	cleanup_old_versions "${PREFIX}/bin" "${BIN_NAME}" "${VERSION}"

	# Remove old am.sh from /etc/profile.d/ if present (legacy cleanup)
	if sudo test -f /etc/profile.d/am.sh; then
		sudo rm -f /etc/profile.d/am.sh
		_log_file "Removed legacy /etc/profile.d/am.sh"
	fi

	# Configure git safe.directory for project repository (fix dubious ownership)
	local project_repo
	project_repo=$(cd "${SCRIPT_DIR}" && git rev-parse --show-toplevel 2>/dev/null || echo "${SCRIPT_DIR}")
	if [[ -d "$project_repo" ]]; then
		sudo git config --system --add safe.directory "$project_repo" 2>/dev/null || true
		_log_file "git safe.directory += $project_repo"
	fi

	# Install bash completion
	local completion_src="${SCRIPT_DIR}/completions/am-completion.bash"
	if [[ -f "$completion_src" ]]; then
		sudo install -d /etc/bash_completion.d
		sudo install -m 0644 "$completion_src" /etc/bash_completion.d/am
		_log_file "bash completion -> /etc/bash_completion.d/am"
	else
		# [log-review] warn:降级自error——补全文件缺失不影响核心功能
		warn "bash 补全文件缺失，跳过"
	fi

	sudo install -d "${DATA_DIR}"
	sudo chmod 0755 "${DATA_DIR}"
	sudo install -d "${CONFIG_DIR}"
	sudo install -d "${CONFIG_DIR}/sudoers.d"

	local sudoers_file="${CONFIG_DIR}/sudoers.d/ai-mirror"
	sudo tee "$sudoers_file" >/dev/null <<SUDOERS
# ai-mirror sudo rules
# Allows members of the ai-mirror group to run ai-mirror-bin commands as root
#
# Security: Defense in depth
# - No wildcards in commands: exact binary path required
# - Subcommands restricted: only listed subcommands are allowed
# - "" suffix allows any arguments; validation enforced by binary
# - Binary validates all paths are under caller's home directory
# - Binary uses O_NOFOLLOW, fs::canonical, and boundary checks
# - Sudoers is the outer gate; binary is the inner validator
#
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} create ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} mkdir ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} touch ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} cp ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} mv ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} cd ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} rm ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} force-destroy ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} health ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} auto-fix-all ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} list ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} config ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} status ""
%ai-mirror ALL=(root) NOPASSWD: ${PREFIX}/bin/${BIN_NAME} update ""
SUDOERS
	sudo chmod 0440 "$sudoers_file"
	sudo chown root:root "$sudoers_file"

	if ! sudo getent group ai-mirror &>/dev/null; then
		sudo groupadd --system ai-mirror 2>/dev/null || true
	fi

	ok "安装 v${VERSION}"
}

# ---- Phase: Summary ----
phase_summary() {
	local VERSION
	VERSION=$(get_version)
	ok "安装完成 v${VERSION}: ${PREFIX}/bin/${WRAPPER_NAME}  (source ~/.bashrc 生效)"

	# Show systemd service/timer status if am-mount-watch was installed
	# NOTE: am-mount-watch.service is Type=oneshot — it runs and exits.
	# "systemctl is-active" returns "inactive" when the service is not
	# currently executing.  Use is-failed to check for crash; check the
	# timer (a daemon) for running status in the normal sense.
	if command -v systemctl &>/dev/null && [[ -f "${PREFIX}/bin/${MOUNT_WATCH_NAME}" ]]; then
		if systemctl is-failed --quiet "am-mount-watch.service" 2>/dev/null; then
			# [log-review] warn:降级自error——service 曾执行失败，可能是配置或环境问题，不影响主程序
			warn "systemd: am-mount-watch.service 曾执行失败 (systemctl is-failed)"
		else
			ok "systemd: am-mount-watch.service 上次执行正常"
		fi
		if systemctl is-active --quiet "am-mount-watch.timer" 2>/dev/null; then
			ok "systemd: am-mount-watch.timer 运行中 (每 5 分钟)"
		else
			# [log-review] warn:降级自error——timer 未运行可能因系统无 systemd 或安装阶段配置失败，属于预期内场景，不影响主程序功能
			warn "systemd: am-mount-watch.timer 未运行"
		fi
		_log_file "systemd status: $(systemctl is-active am-mount-watch.service 2>/dev/null || echo unknown), timer: $(systemctl is-active am-mount-watch.timer 2>/dev/null || echo unknown)"
	fi

	_log_file "Full log: ${INSTALL_LOG}"
}

# ---- Phase: Restart systemd ----
phase_restart_systemd() {
	CURRENT_PHASE="restart_systemd"

	if ! command -v systemctl &>/dev/null; then
		# [log-review] warn:降级自error——systemctl 不可用（容器/非 systemd 系统），无法重启 systemd 服务但主程序功能不受影响
		warn "systemctl 不可用，无法重启 systemd 服务"
		return 1
	fi

	if ! sudo test -f /etc/systemd/system/am-mount-watch.service 2>/dev/null; then
		# [log-review] warn:降级自error——service 文件不存在说明未安装，重启功能不可用但不影响主程序
		warn "am-mount-watch.service 未安装，请先执行 install.sh"
		return 1
	fi

	log "重启 am-mount-watch systemd 服务..."
	sudo systemctl daemon-reload 2>/dev/null || true
	_log_file "systemd: daemon-reload done"

	sudo systemctl restart am-mount-watch.timer 2>/dev/null || true
	_log_file "systemd: timer restarted"

	# 立即执行一次（验证服务可用）
	sudo systemctl start am-mount-watch.service 2>/dev/null || true
	_log_file "systemd: immediate check triggered"

	# 验证状态
	# am-mount-watch.service is Type=oneshot — use is-failed instead of
	# is-active (which returns "inactive" once the service finishes).
	if systemctl is-failed --quiet "am-mount-watch.service" 2>/dev/null; then
		# [log-review] warn:降级自error——service 执行失败，可能是配置或环境问题，不影响主程序
		warn "am-mount-watch.service 启动失败，请手动检查: systemctl status am-mount-watch.service"
	else
		ok "am-mount-watch.service 启动成功"
	fi
	if systemctl is-active --quiet "am-mount-watch.timer" 2>/dev/null; then
		ok "am-mount-watch.timer 运行中 (每 5 分钟)"
	else
		# [log-review] warn:降级自error——timer 可能因环境问题无法启动，但不影响主程序
		warn "am-mount-watch.timer 启动异常，请手动检查: systemctl status am-mount-watch.timer"
	fi
}

# ---- Phase: Clean ----
phase_clean() {
	CURRENT_PHASE="clean"
	require_sudo
	log "Removing installed files..."

	# Remove wrapper
	if sudo test -f "${PREFIX}/bin/${WRAPPER_NAME}"; then
		if ! sudo rm -f "${PREFIX}/bin/${WRAPPER_NAME}"; then
			ERROR_MSG="Failed to remove ${PREFIX}/bin/${WRAPPER_NAME}"
			return 1
		fi
		log "  Removed ${PREFIX}/bin/${WRAPPER_NAME}"
	fi

	# Remove symlink
	if sudo test -L "${PREFIX}/bin/${BIN_NAME}"; then
		if ! sudo rm -f "${PREFIX}/bin/${BIN_NAME}"; then
			ERROR_MSG="Failed to remove symlink ${PREFIX}/bin/${BIN_NAME}"
			return 1
		fi
		log "  Removed symlink ${PREFIX}/bin/${BIN_NAME}"
	fi

	# Remove all versioned binaries
	local versioned_bins
	versioned_bins=$(sudo ls "${PREFIX}/bin/${BIN_NAME}."* 2>/dev/null | grep -E "${BIN_NAME}\.[0-9]+\.[0-9]+$" || true)
	if [[ -n "$versioned_bins" ]]; then
		for f in $versioned_bins; do
			if ! sudo rm -f "$f"; then
				ERROR_MSG="Failed to remove versioned binary: $f"
				return 1
			fi
			log "  Removed $(basename "$f")"
		done
	fi

	# Remove legacy am.sh if present
	if sudo test -f /etc/profile.d/am.sh; then
		sudo rm -f /etc/profile.d/am.sh
		log "  Removed legacy /etc/profile.d/am.sh"
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
			# [log-review] warn:降级自error——data dir 非空可能是运行中产生的数据，跳过删除不影响卸载
			warn "Data dir ${DATA_DIR}/ 非空，跳过删除"
		else
			_log_file "Removed data dir ${DATA_DIR}/"
		fi
	fi

	# Clean up systemd service if am-mount-watch was installed
	# (完整维护逻辑：停止→禁用→删除单元文件→重载daemon→删除binary，内联不委托外部脚本)
	if command -v systemctl &>/dev/null && (sudo test -f "${PREFIX}/bin/${MOUNT_WATCH_NAME}" || sudo test -f /etc/systemd/system/am-mount-watch.service 2>/dev/null || sudo test -f /etc/systemd/system/am-mount-watch.timer 2>/dev/null); then
		log "停止并移除 am-mount-watch systemd 服务（内联）..."
		_log_file "Removing am-mount-watch systemd service (inline)"
		sudo systemctl stop am-mount-watch.timer 2>/dev/null || true
		sudo systemctl disable am-mount-watch.timer 2>/dev/null || true
		sudo systemctl stop am-mount-watch.service 2>/dev/null || true
		sudo systemctl disable am-mount-watch.service 2>/dev/null || true
		sudo rm -f /etc/systemd/system/am-mount-watch.service
		sudo rm -f /etc/systemd/system/am-mount-watch.timer
		sudo systemctl daemon-reload 2>/dev/null || true
		_log_file "systemd: service and timer stopped, disabled, removed"
		sudo rm -f "${PREFIX}/bin/${MOUNT_WATCH_NAME}"
		log "  Removed am-mount-watch binary + systemd 服务"
		ok "am-mount-watch systemd 服务已清理"
	elif [[ -f "${PREFIX}/bin/${MOUNT_WATCH_NAME}" ]]; then
		# systemctl 不可用，仅删除 binary
		sudo rm -f "${PREFIX}/bin/${MOUNT_WATCH_NAME}"
		log "  Removed am-mount-watch binary（systemctl 不可用，跳过 systemd）"
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
		ok "构建完成: ${BUILD_DIR}/bin/"
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
	--restart-systemd)
		CURRENT_PHASE="restart_systemd"
		phase_setup || return $?
		phase_restart_systemd || return $?
		;;
	--help | -h)
		echo "Usage: bash $0 [--build|--clean|--restart-systemd|--help]"
		echo ""
		echo "  (default)       Build and install (sudo used only when needed)"
		echo "  --build         Build and verify only, no sudo needed"
		echo "  --clean         Remove all installed files (sudo needed)"
		echo "  --restart-systemd  Restart am-mount-watch systemd service + timer"
		echo "  --help          Show this help"
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

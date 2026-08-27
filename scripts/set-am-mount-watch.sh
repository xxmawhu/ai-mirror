#!/usr/bin/env bash
# set-am-mount-watch.sh — 安装 am-mount-watch systemd 服务
#
# 用法:
#   bash scripts/set-am-mount-watch.sh               # 安装+启动+开机启动+立即检查一次
#   bash scripts/set-am-mount-watch.sh --uninstall    # 移除服务
#   bash scripts/set-am-mount-watch.sh --status       # 查看运行状态
#
# 这个脚本:
#   1. 编译 am-mount-watch binary（如果不存在）
#   2. 安装到 /usr/local/bin/
#   3. 创建 systemd service + timer 单元文件
#   4. 启用并启动 timer
#   5. 立即检查一次
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-release"
LOG_DIR="${PROJECT_DIR}/log/hook"
mkdir -p "$LOG_DIR"

BIN_NAME="am-mount-watch"
BIN_SOURCE="${BUILD_DIR}/bin/${BIN_NAME}"
BIN_TARGET="/usr/local/bin/${BIN_NAME}"

SERVICE_NAME="am-mount-watch.service"
TIMER_NAME="am-mount-watch.timer"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}"
TIMER_FILE="/etc/systemd/system/${TIMER_NAME}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info() { echo -e "${CYAN}  ℹ️  $1${NC}"; }
log_ok() { echo -e "${GREEN}  ✅ $1${NC}"; }
log_warn() { echo -e "${YELLOW}  ⚠️  $1${NC}"; }
log_err() { echo -e "${RED}  ❌ $1${NC}"; }

# ---- 前置检查 ----

check_root() {
	if [[ $EUID -ne 0 ]]; then
		log_warn "部分操作需要 root 权限（安装 binary、配置 systemd）"
		log_info "尝试 sudo 自动提权..."
		exec sudo bash "$0" "$@"
		exit 1
	fi
}

usage() {
	echo "用法: bash $0 [--uninstall|--status]"
	exit 1
}

# ---- 编译 binary ----

build_binary() {
	if [[ -x "$BIN_SOURCE" ]]; then
		log_info "Binary 已存在: $BIN_SOURCE"
		return 0
	fi

	log_info "编译 am-mount-watch..."

	# 如果 build-release 不存在，cmake 配置
	if [[ ! -d "$BUILD_DIR" ]]; then
		cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >>"${LOG_DIR}/mount-watch-build.log" 2>&1
	fi

	if cmake --build "$BUILD_DIR" --target "$BIN_NAME" -j"$(nproc)" >>"${LOG_DIR}/mount-watch-build.log" 2>&1; then
		log_ok "编译成功"
	else
		log_err "编译失败，详见: ${LOG_DIR}/mount-watch-build.log"
		exit 1
	fi
}

# ---- 安装 binary ----

install_binary() {
	if [[ ! -x "$BIN_SOURCE" ]]; then
		log_err "Binary 不存在: $BIN_SOURCE，请先编译"
		exit 1
	fi

	cp "$BIN_SOURCE" "$BIN_TARGET"
	chmod 755 "$BIN_TARGET"
	log_ok "已安装到 $BIN_TARGET"
}

# ---- 创建 systemd 单元 ----

create_units() {
	# Service 单元
	cat >"$SERVICE_FILE" <<UNIT_EOF
[Unit]
Description=am-mount-watch — AI mirror mount health checker & auto-repair
Documentation=https://github.com/maxx/ai-mirror
After=local-fs.target

[Service]
Type=oneshot
ExecStart=${BIN_TARGET}
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
	log_ok "已创建 $SERVICE_FILE"

	# Timer 单元（每 5 分钟运行一次）
	cat >"$TIMER_FILE" <<UNIT_EOF
[Unit]
Description=am-mount-watch timer (every 5 min)
Requires=${SERVICE_NAME}

[Timer]
OnCalendar=*:0/5
Persistent=true
RandomizedDelaySec=30

[Install]
WantedBy=timers.target
UNIT_EOF
	log_ok "已创建 $TIMER_FILE"
}

# ---- 启用并启动 ----

enable_and_start() {
	systemctl daemon-reload

	systemctl enable "$TIMER_NAME"
	log_ok "已启用开机启动: $TIMER_NAME"

	systemctl start "$TIMER_NAME"
	log_ok "已启动 timer: $TIMER_NAME"

	# 立即执行一次（oneshot，不等 timer 周期）
	log_info "立即执行一次检查..."
	systemctl start "$SERVICE_NAME" || true
}

# ---- 检查一次 ----

run_check() {
	log_info "运行 am-mount-watch..."
	if "${BIN_TARGET}" 2>&1; then
		log_ok "所有 mount 健康"
	else
		local rc=$?
		if [[ $rc -eq 1 ]]; then
			log_ok "stale mount 已清理"
		else
			# warn:降级自error——无法自动修复的 mount 问题不影响脚本执行，仅提示用户手动处理
			log_warn "存在无法修复的问题（exit=$rc）"
		fi
	fi
}

# ---- 查看状态 ----

show_status() {
	echo ""
	echo -e "${CYAN}=== Service Status ===${NC}"
	systemctl status "$SERVICE_NAME" 2>&1 || true
	echo ""
	echo -e "${CYAN}=== Timer Status ===${NC}"
	systemctl status "$TIMER_NAME" 2>&1 || true
	echo ""
	echo -e "${CYAN}=== Timer Schedule ===${NC}"
	systemctl list-timers --all | grep -i "mount-watch" || echo "  (no timer found)"
}

# ---- 卸载 ----

uninstall() {
	log_info "停止并禁用 timer..."
	systemctl stop "$TIMER_NAME" 2>/dev/null || true
	systemctl disable "$TIMER_NAME" 2>/dev/null || true
	systemctl stop "$SERVICE_NAME" 2>/dev/null || true
	systemctl disable "$SERVICE_NAME" 2>/dev/null || true

	rm -f "$TIMER_FILE" "$SERVICE_FILE"
	systemctl daemon-reload
	log_ok "已移除 systemd 单元"

	if [[ -f "$BIN_TARGET" ]]; then
		rm -f "$BIN_TARGET"
		log_ok "已移除 binary: $BIN_TARGET"
	fi
}

# ---- Main ----

CMD="${1:-}"

case "$CMD" in
--uninstall)
	check_root "$@"
	uninstall
	;;
--status)
	show_status
	;;
-h | --help)
	usage
	;;
"")
	# 默认：安装+启动
	check_root "$@"
	build_binary
	install_binary
	create_units
	enable_and_start
	run_check
	show_status
	echo ""
	log_ok "am-mount-watch 安装完成，每 5 分钟自动检查 mount 健康状态"
	;;
*)
	usage
	;;
esac

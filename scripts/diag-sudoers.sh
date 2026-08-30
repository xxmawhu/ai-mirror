#!/usr/bin/env bash
#
# diag-sudoers.sh — 一键诊断 am 免密链路（组 → 规则文件 → 实际裁决 三层）
#
# 背景：`am <cmd>` 提示 `[sudo] password for maxx:` 时，故障一定出在
# sudoers NOPASSWD 规则层面 —— wrapper 组检查通过后，sudo 按 last-match
# 语义裁决要密码（NOPASSWD 规则缺失/被 `ALL=(ALL) ALL` 遮蔽/未部署最新版）。
# 本脚本在任意服务器只读诊断，逐层输出断点，无需猜测。
#
# 用法（root 或 ai-mirror 组员均可；无读取/查询权限的层输出 [skip] 不误报）：
#   sudo bash scripts/diag-sudoers.sh [用户名]   # 显式指定目标用户
#   sudo bash scripts/diag-sudoers.sh            # 自动：SUDO_USER 或遍历组员
#
# 退出码：0=全部检查通过 1=存在确认断点 2=存在未验证（skip）项 —— 供自动巡检判定
#
# [设计说明] 本脚本刻意不用 `set -e`：诊断须累积全部断点逐层输出，
# 单个检查失败不应中断后续诊断。set -u 防未定义变量、pipefail 保管道退出码。
#
set -uo pipefail

PREFIX="${AI_MIRROR_PREFIX:-/usr/local}"
BIN_NAME="${BIN_NAME:-ai-mirror-bin}"
GROUP="${GROUP:-ai-mirror}"
SUDOERS_DIR="/etc/sudoers.d"
# 子命令白名单：优先复用 ensure-user-sudoers.sh 的 AI_MIRROR_SUBCMDS（单一事实来源）
SUB_COMMANDS=()
if declare -p AI_MIRROR_SUBCMDS &>/dev/null 2>&1; then
	SUB_COMMANDS=("${AI_MIRROR_SUBCMDS[@]}")
else
	SUB_COMMANDS=(create mkdir touch cp mv cd rm force-destroy health auto-fix-all list config status update frz)
fi
# 只读裁决探针子命令（白名单内、无副作用 —— 用于 sudo -n 实际执行级裁决验证）
PROBE_SUB="health"

IS_ROOT=false
[[ $EUID -eq 0 ]] && IS_ROOT=true

SUMMARY_FAIL=0
SUMMARY_SKIP=0
pass() { echo "  [OK]   $*"; }
fail() {
	echo "  [FAIL] $*"
	SUMMARY_FAIL=1
}
# [skip]：环境权限不足无法确证，不计入断点（避免非 root 误报）
skip() {
	echo "  [skip] $*"
	SUMMARY_SKIP=1
}
info() { echo "  [info] $*"; }

# rg 优先、缺失降级 grep（平台现代 CLI 规范；诊断脚本运行于任一台生产机）
grep_rg_silent() { # 用法: grep_rg_silent <pattern> <file>
	local pat="$1" f="$2"
	if command -v rg &>/dev/null; then
		rg -q "$pat" "$f" 2>/dev/null
	else
		grep -q "$pat" "$f" 2>/dev/null
	fi
}
# 输出文本匹配（rg 优先降级 grep）
echo_matches() {
	local txt="$1" pat="$2"
	if command -v rg &>/dev/null; then
		echo "$txt" | rg -q "$pat"
	else
		echo "$txt" | grep -Eq "$pat"
	fi
}

# ---- 收集目标用户（去重） ----
TARGETS=()
if [[ $# -ge 1 ]]; then
	TARGETS+=("$1")
else
	if [[ -n "${SUDO_USER:-}" ]]; then
		TARGETS+=("$SUDO_USER")
	fi
	while IFS=: read -r _gt _gx _gid _gmem; do
		[[ -n "${_gmem:-}" ]] || continue
		IFS=',' read -r -a _garr <<<"$_gmem"
		for _u in "${_garr[@]}"; do
			if [[ " ${TARGETS[*]} " != *" $_u "* ]]; then
				TARGETS+=("$_u")
			fi
		done
	done < <(getent group "$GROUP" 2>/dev/null || true)
	[[ ${#TARGETS[@]} -gt 0 ]] || TARGETS+=("$(id -un)")
fi

# ---- 层 1: 组与用户归属 ----
echo "===== 层 1: 组与用户归属 ====="
if getent group "$GROUP" >/dev/null 2>&1; then
	pass "组 $GROUP 存在: $(getent group "$GROUP" | cut -d: -f4)"
else
	fail "组 $GROUP 不存在（修复: sudo groupadd --system $GROUP; sudo usermod -aG $GROUP $USER）"
fi
for _t in "${TARGETS[@]}"; do
	if getent passwd "$_t" >/dev/null 2>&1; then
		if getent group "$GROUP" | cut -d: -f4 | tr ',' '\n' | grep -qx "$_t"; then
			pass "$_t 属于 $GROUP 组（wrapper 组检查可通过）"
		else
			fail "$_t 不属于 $GROUP 组 —— wrapper 会直接拒绝（错误信息不是 sudo 密码提示）"
		fi
	else
		fail "用户 $_t 不存在"
	fi
done

# ---- 层 2: sudoers 规则文件 ----
echo "===== 层 2: sudoers 规则文件 ====="
if [[ -d "$SUDOERS_DIR" ]]; then
	if [[ -r "$SUDOERS_DIR" ]]; then
		info "$SUDOERS_DIR 文件列表:"
		# shellcheck disable=SC2012 # 目录列表需直观展示权限/属主，ls 语义优于 find
		ls -la "$SUDOERS_DIR" | sed 's/^/    /'
	else
		skip "无权限读取 $SUDOERS_DIR（目录 0640 root 常见）—— 请以 root 运行获得完整文件检查"
	fi
	if [[ -r /etc/sudoers ]]; then
		# 兼容 sudo 1.9.6+ 的 #includedir 与 @includedir 两种语法
		if grep_rg_silent '^[#@]includedir[[:space:]]*/etc/sudoers.d' /etc/sudoers; then
			pass "主 /etc/sudoers 已 include $SUDOERS_DIR"
		else
			fail "主 /etc/sudoers 未包含 '#/includedir /etc/sudoers.d' —— 本目录规则永远不会被加载"
		fi
	else
		skip "无权限读取 /etc/sudoers（0440 root）—— include 检查需 root"
	fi
else
	fail "$SUDOERS_DIR 不存在（修复: sudo mkdir -p $SUDOERS_DIR）"
fi
# 组规则 zzz-ai-mirror（2026-08-27 起的标准兜底文件）
_GRULE="${SUDOERS_DIR}/zzz-ai-mirror"
if [[ -r "$_GRULE" ]]; then
	pass "组规则存在: $_GRULE"
elif [[ -e "$_GRULE" ]]; then
	skip "组规则存在但无读取权限: $_GRULE（root 可读）"
else
	fail "组规则缺失: $_GRULE —— 未执行最新 install.sh（含 2026-08-27 zzz- 方案）"
fi
# 用户级规则（方案 A MARKER 块）
for _t in "${TARGETS[@]}"; do
	_uf="${SUDOERS_DIR}/${_t}"
	if [[ -r "$_uf" ]]; then
		if grep -q "MARKER_ai-mirror user-level NOPASSWD" "$_uf" 2>/dev/null; then
			pass "用户级规则已就位: $_uf（MARKER 块存在，同文件后行=最后命中）"
		else
			fail "用户级规则缺失: $_uf 存在但无 MARKER 块（install.sh 的 ensure-user-sudoers.sh 未执行或版本过旧）"
		fi
	elif [[ -e "$_uf" ]]; then
		skip "用户级规则文件存在但无读取权限: $_uf"
	else
		fail "用户级规则缺失: $_uf 不存在 —— 2026-08-30 起 install.sh 会为该组员创建用户级文件"
	fi
done

# ---- 层 3: 实际裁决（sudo -n 实际执行级，last-match 真裁决） ----
# 说明：`sudo -l`（无论带不带命令）只列候选规则，`ALL=(ALL) ALL` 遮蔽时输出
# 仍含 NOPASSWD 行 → 假阳性。本层以 `sudo -n -u root <bin> <probe>`（只读
# health，白名单内）实际执行：免密生效 exit 0；被遮蔽/无规则报 password 错误
# exit 非 0。sudo -n 强制非交互，绝不等待密码输入。
echo "===== 层 3: 实际裁决（sudo -n 实际执行 $PROBE_SUB） ====="
for _t in "${TARGETS[@]}"; do
	echo "  --- 用户 $_t ---"
	# 1) 存在性：直接读规则文件（组规则 + 用户级文件）逐子命令核对。
	#    不用 `sudo -l -U <user> <bin> <sub>` 输出字符串匹配 —— sudo -l 对
	#    symlink 命令会 realpath 展开（输出 ai-mirror-bin.0.1 而非 ai-mirror-bin）
	#    → 误判无规则（2026-08-30 gpu-server-98-4 部署失败即此根因）。读文件内容
	#    无 realpath 噪声，且与 install.sh 强校验同一实现。
	_rules=""
	if $IS_ROOT; then
		_rules=$(cat "${SUDOERS_DIR}/zzz-ai-mirror" 2>/dev/null || true)
		if [[ -f "${SUDOERS_DIR}/${_t}" ]]; then
			_rules+=$'\n'"$(cat "${SUDOERS_DIR}/${_t}" 2>/dev/null || true)"
		fi
	elif command -v sudo &>/dev/null && sudo -n -l -U "$_t" >/dev/null 2>&1; then
		_rules=$(sudo cat "${SUDOERS_DIR}/zzz-ai-mirror" 2>/dev/null || true)
		if sudo test -f "${SUDOERS_DIR}/${_t}" 2>/dev/null; then
			_rules+=$'\n'"$(sudo cat "${SUDOERS_DIR}/${_t}" 2>/dev/null || true)"
		fi
	fi
	if [[ -n "$_rules" ]]; then
		local_bad=()
		for _s in "${SUB_COMMANDS[@]}"; do
			if ! echo_matches "$_rules" "NOPASSWD:.*${BIN_NAME} ${_s}"; then
				local_bad+=("${_s}(缺行)")
			fi
		done
		if [[ ${#local_bad[@]} -gt 0 ]]; then
			fail "$_t 规则存在性未通过: ${local_bad[*]}"
			info "  白名单子命令集合不一致（install.sh / ensure-user-sudoers.sh / diag 需同源）"
		else
			pass "$_t 规则文件存在性完整（${#SUB_COMMANDS[@]} 子命令齐全）"
		fi
	else
		skip "无法读取规则文件（非 root 且无 sudo list 权限）—— 存在性检查跳过"
	fi
	# 2) 终极裁决：实际执行（root 上下文 runuser 切用户；否则以调用者身份）
	_po=""
	_pr=0
	if $IS_ROOT && command -v runuser &>/dev/null && [[ "$(id -un)" != "$_t" ]]; then
		_po=$(runuser -u "$_t" -- sudo -n -u root "${PREFIX}/bin/${BIN_NAME}" "$PROBE_SUB" 2>&1)
		_pr=$?
	elif command -v sudo &>/dev/null; then
		_po=$(sudo -n -u root "${PREFIX}/bin/${BIN_NAME}" "$PROBE_SUB" 2>&1)
		_pr=$?
	else
		_pr=-1
	fi
	if [[ $_pr -eq 0 ]]; then
		pass "$_t 实际裁决通过（免密执行 $PROBE_SUB 成功）"
	elif [[ $_pr -eq -1 ]]; then
		skip "系统无 sudo 命令 —— 裁决验证需 root 或 sudo"
	elif echo_matches "$_po" "password|terminal is required"; then
		fail "$_t 实际裁决失败（NOPASSWD 未生效 → sudo 要求密码）"
		info "  根因通常是: 规则被 ALL=(ALL) ALL 遮蔽 或 层 2 规则缺失（看上方 [FAIL]/[skip] 行）"
	else
		fail "$_t 实际执行 $PROBE_SUB 非零（exit=$_pr）：$_po"
	fi
done

# ---- 结论 ----
echo ""
echo "===== 结论 ====="
if [[ "$SUMMARY_FAIL" -eq 0 ]]; then
	if [[ "$SUMMARY_SKIP" -eq 0 ]]; then
		echo "[OK] am 免密链路完整，am <command> 应免密执行"
	else
		echo "[OK] 未发现确认断点，但有 [skip] 项未能验证（sudo bash scripts/diag-sudoers.sh 以 root 复核）"
	fi
else
	echo ""
	echo "[FAIL] 发现确认断点（见上方 [FAIL] 行）。修复通常只需以 root 重跑最新 install.sh，或单独执行:"
	echo "       sudo bash scripts/ensure-user-sudoers.sh   （补用户级规则）"
	echo "       sudo bash install.sh                        （全量重装 + 实际裁决强校验）"
fi
# 退出码：1=确认断点 2=仅 skip（未验证） 0=全通过
[[ "$SUMMARY_FAIL" -eq 0 ]] || exit 1
[[ "$SUMMARY_SKIP" -eq 0 ]] || exit 2
exit 0

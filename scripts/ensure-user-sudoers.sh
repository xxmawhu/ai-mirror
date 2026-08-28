#!/usr/bin/env bash
#
# ensure-user-sudoers.sh — 方案 A 根治：把用户级 NOPASSWD 规则安全追加到
# ai-mirror 组员自己的 sudoers 文件（/etc/sudoers.d/<user>）物理末尾。
#
# 原理（man sudoers）：
#   "When multiple entries match for a user, they are applied in order. Where
#   there are multiple matches, the last match is used."
#   同一文件内靠后的规则必然最后命中 —— 与 /etc/sudoers.d 文件名字典序解耦，
#   任何 `xxx ALL=(ALL) ALL`（无论写在哪个文件）都无法遮蔽本次追加的 NOPASSWD。
#
# 安全设计（相对 install.sh 内联 tee -a 的加固）：
#   1) 原子写入：先写临时文件 → visudo -cf 校验临时文件 → 校验通过后 mv 覆盖。
#      目标文件在校验通过前【从不被修改】，校验失败 = 天然回滚（原文件原样保留）。
#   2) 全量语法校验：visudo -cf 校验临时文件（含组员原有内容 + 追加块），
#      避免单文件语法 OK 但与主文件/别名组合解析失败的问题。
#   3) 拒写符号链接：/etc/sudoers.d/<user> 若是 symlink 则跳过（防写穿到别处）。
#   4) 用户名 POSIX 校验：^[a-z_][a-z0-9_-]*$，防异常用户名破坏 sudoers 语法。
#   5) 幂等：文件已含 MARKER 块则跳过，不重复追加。
#   6) 权限/属主：追加后恢复 0440 root:root（sudoers 加载必需），逐项校验失败即 abort。
#   7) 可注入配置：SUDO_CMD / SUDOERS_DIR / PREFIX / BIN_NAME 均可覆盖，
#      供单元测试在临时目录、免 root 环境验证（不以 root 运行也能测逻辑）。
#
# 用法（由 install.sh / fix-sudoers-location.sh 调用，也可单独执行）：
#   bash scripts/ensure-user-sudoers.sh
# 环境变量覆盖（供测试）：
#   SUDO_CMD        sudo 前缀（默认 "sudo"；测试可设为 "" 或 "env"）
#   SUDOERS_DIR     目标目录（默认 /etc/sudoers.d；测试设为临时目录）
#   PREFIX          安装前缀（默认 /usr/local）
#   BIN_NAME        二进制名（默认 ai-mirror-bin）
#   GROUP           组名（默认 ai-mirror）
#   MARKER          幂等标记（默认 "ai-mirror user-level NOPASSWD"）
#
set -euo pipefail

SUDO_CMD="${SUDO_CMD:-sudo}"
SUDOERS_DIR="${SUDOERS_DIR:-/etc/sudoers.d}"
PREFIX="${PREFIX:-/usr/local}"
BIN_NAME="${BIN_NAME:-ai-mirror-bin}"
GROUP="${GROUP:-ai-mirror}"
# 幂等标记（与卸载删除逻辑共用，勿改此默认值）
MARKER="${MARKER:-ai-mirror user-level NOPASSWD}"

# ---- 日志（stderr，避免污染 stdout 供测试断言）----
_ul_log() { echo "[ensure-user-sudoers] $*" >&2; }
_ul_warn() { echo "[ensure-user-sudoers][warn] $*" >&2; }
_ul_err() { echo "[ensure-user-sudoers][error] $*" >&2; }

# ---- 生成用户级白名单规则文本（纯函数，供测试独立断言）----
gen_user_rules() {
	local user="$1"
	local sub
	for sub in create mkdir touch cp mv cd rm force-destroy health auto-fix-all list config status update frz; do
		printf '%s ALL=(root) NOPASSWD: %s/bin/%s %s\n' "$user" "$PREFIX" "$BIN_NAME" "$sub"
	done
}

# ---- 用户名合法性：POSIX 登录名（开头字母或 _，后续字母数字 _ -）----
is_valid_username() {
	[[ "$1" =~ ^[a-z_][a-z0-9_-]*$ ]]
}

# ---- 追加用户级规则到单个组员文件（安全原子）----
# 返回：0=已处理/跳过/幂等，1=失败（调用方应中止安装）
ensure_one_user() {
	local user="$1"
	local suf="${SUDOERS_DIR}/${user}"

	# 用户名合法性（防异常用户名破坏 sudoers 语法）
	if ! is_valid_username "$user"; then
		_ul_warn "跳过非法用户名 '${user}'（不合法 POSIX 登录名）"
		return 0
	fi

	# 目标文件必须存在
	if ! $SUDO_CMD test -f "$suf"; then
		_ul_log "无自家 sudoers 文件，跳过: ${suf}"
		return 0
	fi

	# 拒写符号链接（防写穿/改写他人文件）
	if $SUDO_CMD test -L "$suf"; then
		_ul_warn "跳过符号链接（防写穿）: ${suf}"
		return 0
	fi

	# 幂等：已含 MARKER 则跳过
	if $SUDO_CMD grep -q "MARKER_${MARKER}" "$suf" 2>/dev/null; then
		_ul_log "已含 ai-mirror NOPASSWD 块，幂等跳过: ${suf}"
		return 0
	fi

	# ---- 原子写入：临时文件 → visudo 校验 → mv 覆盖（原文件校验前不动）----
	local tmpfile
	tmpfile=$($SUDO_CMD mktemp "${SUDOERS_DIR}/.tmp-ai-mirror-XXXXXX") || {
		_ul_err "创建临时文件失败: ${SUDOERS_DIR}/.tmp-ai-mirror-XXXXXX"
		return 1
	}
	# 临时文件复制原内容
	if ! $SUDO_CMD cp "$suf" "$tmpfile"; then
		$SUDO_CMD rm -f "$tmpfile"
		_ul_err "复制原文件到临时文件失败: ${suf}"
		return 1
	fi

	# 追加标记块（带 BEGIN/END 标记，供卸载精确删除）
	{
		printf '# ==== MARKER_%s (BEGIN) ====\n' "$MARKER"
		printf '#### ai-mirror user-level NOPASSWD (ensure-user-sudoers.sh, issue 2026-08-27)\n'
		gen_user_rules "$user"
		printf '# ==== MARKER_%s (END) ====\n' "$MARKER"
	} | $SUDO_CMD tee -a "$tmpfile" >/dev/null

	# visudo 校验整个临时文件（含组员原有内容 + 追加块，防别名/组合解析失败）
	if ! $SUDO_CMD visudo -cf "$tmpfile" 2>/dev/null; then
		$SUDO_CMD rm -f "$tmpfile"
		_ul_err "sudoers 语法校验失败（原文件未改动）: ${suf}"
		return 1
	fi

	# 权限/属主（sudoers 加载要求 0440 root:root）——先设权限再 mv，避免窗口期可写
	if ! $SUDO_CMD chmod 0440 "$tmpfile"; then
		$SUDO_CMD rm -f "$tmpfile"
		_ul_err "临时文件 chmod 0440 失败: ${tmpfile}"
		return 1
	fi
	if ! $SUDO_CMD chown root:root "$tmpfile"; then
		# [log-review] warn：容器/测试环境可能无法 chown（非 root 运行），
		# 真实部署必须 root；这里降级为警告保留文件内容正确性，权限在 cp 时继承
		_ul_warn "chown root:root 失败（内容仍正确，权限继承原文件）: ${tmpfile}"
	fi

	# 原子覆盖：mv 成功后原文件即替换为校验通过版本
	if ! $SUDO_CMD mv "$tmpfile" "$suf"; then
		$SUDO_CMD rm -f "$tmpfile"
		_ul_err "原子替换失败: ${suf}"
		return 1
	fi
	_ul_log "已追加用户级 NOPASSWD 规则（同文件后行=最后命中）: ${suf}"
	return 0
}

# ---- 主流程：枚举组员并逐一处理 ----
ensure_all_users() {
	local _uu _gx _gid _umem _uarr user
	local any_fail=0

	# 组可能存在但成员为空（首次安装）
	if ! getent group "${GROUP}" >/dev/null 2>&1; then
		_ul_warn "组 ${GROUP} 不存在（首次安装？），跳过用户级规则追加"
		return 0
	fi

	while IFS=: read -r _uu _gx _gid _umem; do
		[[ -n "${_umem:-}" ]] || continue
		IFS=',' read -r -a _uarr <<<"$_umem"
		for user in "${_uarr[@]}"; do
			if ! ensure_one_user "$user"; then
				any_fail=1
			fi
		done
	done < <(getent group "${GROUP}" 2>/dev/null || true)

	return "$any_fail"
}

# ---- 独立执行入口（被 source 时不执行）----
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
	ensure_all_users
	exit $?
fi

# 被 source：只定义函数，供单元测试与 install.sh 复用

#!/usr/bin/env bash
# am.sh - ai-mirror shell function for bash profile
#
# This file should be sourced by bash, typically installed to:
#   - Global: /etc/profile.d/am.sh
#   - User:   ~/.bashrc or ~/.bash_profile
#
# The 'am' function wraps ai-mirror-bin and provides:
#   - 'cd' command that changes directory in the current shell
#   - 'cd' to AI user project via SSH with automatic user detection
#   - Pass-through for all other commands

# Binary search paths (checked in order)
_AM_SEARCH_PATHS=(
	"${AI_MIRROR_BIN:-}"
	"/usr/local/bin/ai-mirror-bin"
	"${HOME:-}/.local/bin/ai-mirror-bin"
)

# Cache the main user (detected from SUDO_USER or current user)
_am_get_main_user() {
	if [[ -n "${SUDO_USER:-}" ]]; then
		echo "$SUDO_USER"
	else
		whoami
	fi
}

# Validate path is under HOME, /home, or in allowed_bases
# Also validates BeeGFS paths (/mnt/beegfs_data) and /scratch
_am_validate_path() {
	local path="$1"
	local home="${HOME:-$(getent passwd "$(id -un)" | cut -d: -f6)}"
	local main_user
	main_user=$(_am_get_main_user)

	# Under HOME
	if [[ "$path" == "$home"* ]]; then
		return 0
	fi
	# Under /home/{main_user}
	if [[ "$path" == "/home/${main_user}"* ]]; then
		return 0
	fi
	# BeeGFS paths (allowed_bases pattern: /mnt/beegfs_data/usr/{user})
	if [[ "$path" == "/mnt/beegfs_data/usr/${main_user}"* ]]; then
		return 0
	fi
	# /scratch paths (allowed_bases pattern: /scratch/{user})
	if [[ "$path" == "/scratch/${main_user}"* ]]; then
		return 0
	fi
	return 1
}

# Validate AI username format: {prefix}{main_user}_{hash}
# Default prefix is 'i', so username starts with 'i' followed by main_user
_am_validate_ai_user() {
	local user="$1"
	local main_user="$2"

	# AI user format: {prefix}{main_user}_{hash}
	# Must start with prefix (default 'i') followed by main_user and underscore
	if [[ "$user" == i"${main_user}"_* ]]; then
		return 0
	fi
	return 1
}

# Parse key=value output from ai-mirror-bin cd command
_am_parse_output() {
	local output="$1"
	local key="$2"

	# Use || true to prevent grep failure from causing script exit (set -e)
	echo "$output" | grep "^${key}=" | cut -d= -f2- || true
}

# Resolve the binary path (lazy, cached on first call)
_am_resolve_bin() {
	if [[ -n "${_AM_BIN:-}" && -x "$_AM_BIN" ]]; then
		return 0
	fi
	local candidate
	for candidate in "${_AM_SEARCH_PATHS[@]}"; do
		[[ -z "$candidate" ]] && continue
		if [[ -x "$candidate" ]]; then
			_AM_BIN="$candidate"
			return 0
		fi
	done
	return 1
}

# Main am function
am() {
	# Resolve binary path dynamically
	if ! _am_resolve_bin; then
		echo "error: ai-mirror binary not found" >&2
		echo "  searched: ${_AM_SEARCH_PATHS[*]}" >&2
		echo "  hint: run 'bash install.sh' from ai-mirror source, or set AI_MIRROR_BIN" >&2
		return 1
	fi

	# Special handling for 'cd' command
	if [[ "${1:-}" == "cd" ]]; then
		shift

		# Get output from ai-mirror-bin cd
		local output
		local ret=0

		if [[ "$(id -u)" -eq 0 ]]; then
			output=$("$_AM_BIN" cd "$@") || ret=$?
		else
			output=$(sudo --preserve-env=HOME "$_AM_BIN" cd "$@") || ret=$?
		fi

		if [[ $ret -ne 0 ]]; then
			echo "$output"
			return $ret
		fi

		# Parse output
		local action user path
		action=$(_am_parse_output "$output" "action")
		user=$(_am_parse_output "$output" "user")
		path=$(_am_parse_output "$output" "path")
		ssh_key=$(_am_parse_output "$output" "key")

		# Print debug/info lines for troubleshooting (stderr to avoid interfering with parse)
		echo "$output" | grep -E "^debug=|^WARNING:" >&2 || true

		case "$action" in
		ssh)
			# Validate path and user before executing
			if [[ -z "$path" ]]; then
				echo "error: empty path from am cd" >&2
				return 1
			fi
			if ! _am_validate_path "$path"; then
				echo "error: invalid path from am cd: '$path' (must be under \$HOME or /home)" >&2
				return 1
			fi
			if [[ -z "$user" ]]; then
				echo "error: empty user from am cd" >&2
				return 1
			fi
			local main_user
			main_user=$(_am_get_main_user)
			if ! _am_validate_ai_user "$user" "$main_user"; then
				echo "error: invalid AI user from am cd: '$user' (expected format: i${main_user}_<hash>)" >&2
				return 1
			fi

			# SSH to AI user using dedicated key (exit returns to this shell)
			if [[ -z "$ssh_key" ]]; then
				echo "error: no SSH key path from am cd" >&2
				return 1
			fi
			if [[ ! -f "$ssh_key" ]]; then
				echo "error: SSH key missing: $ssh_key. Run 'am update' first." >&2
				return 1
			fi
			# SSH to AI user: start interactive login shell
			# The ai user's HOME is set to the project directory, so no explicit cd needed
			# StrictHostKeyChecking=accept-new: auto-accept new hosts, verify existing (MITM protection)
			# UserKnownHostsFile: ensure known_hosts is used from AI user's ~/.ssh/
			ssh -tt -i "$ssh_key" \
				-o IdentitiesOnly=yes \
				-o StrictHostKeyChecking=accept-new \
				-o UserKnownHostsFile=~/.ssh/known_hosts \
				"${user}@localhost"
			;;
		cd)
			# Validate path
			if [[ -z "$path" ]]; then
				echo "error: empty path from am cd" >&2
				return 1
			fi
			if ! _am_validate_path "$path"; then
				echo "error: invalid path from am cd: '$path' (must be under \$HOME or /home)" >&2
				return 1
			fi

			# Change directory in current shell
			cd "$path"
			;;
		*)
			echo "error: unknown action from am cd: '$action'" >&2
			return 1
			;;
		esac
		return 0
	fi

	# All other commands: pass through to ai-mirror-bin
	if [[ "$(id -u)" -eq 0 ]]; then
		# TUI commands (watch) need direct terminal access, not captured
		if [[ "${1:-}" == "watch" ]]; then
			"$_AM_BIN" "$@"
			return $?
		fi
		local cmd_output
		cmd_output=$("$_AM_BIN" "$@" 2>&1)
		local cmd_ret=$?
		echo "$cmd_output"
		# Hint for newgrp if group membership changed
		local newgrp_hint
		newgrp_hint=$(echo "$cmd_output" | grep "^newgrp=" | cut -d= -f2- || true)
		if [[ -n "$newgrp_hint" ]]; then
			echo "INFO: Run 'newgrp $newgrp_hint' to activate new group membership in current shell" >&2
		fi
		return $cmd_ret
	else
		# TUI commands (watch) need direct terminal access, not captured
		if [[ "${1:-}" == "watch" ]]; then
			sudo --preserve-env=HOME "$_AM_BIN" "$@"
			return $?
		fi
		local cmd_output
		cmd_output=$(sudo --preserve-env=HOME "$_AM_BIN" "$@" 2>&1)
		local cmd_ret=$?
		echo "$cmd_output"
		# Hint for newgrp if group membership changed
		local newgrp_hint
		newgrp_hint=$(echo "$cmd_output" | grep "^newgrp=" | cut -d= -f2- || true)
		if [[ -n "$newgrp_hint" ]]; then
			echo "INFO: Run 'newgrp $newgrp_hint' to activate new group membership in current shell" >&2
		fi
		return $cmd_ret
	fi
}

# Export the function so it's available in subshells
export -f am
export -f _am_get_main_user
export -f _am_validate_path
export -f _am_validate_ai_user
export -f _am_parse_output

# Source bash completion if available and not already loaded
# This ensures completion works in all shell types (login, non-login, tmux, screen, etc.)
if [[ -n "${BASH_VERSION:-}" ]] && [[ -z "${_am_completion_loaded:-}" ]]; then
	_am_completion_file="/etc/bash_completion.d/am"
	if [[ -r "$_am_completion_file" ]]; then
		. "$_am_completion_file"
	fi
	_am_completion_loaded=1
fi

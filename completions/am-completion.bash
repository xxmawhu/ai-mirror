# bash completion for am / ai-mirror
# Install:
#   source completions/am-completion.bash
#   or copy to ~/.local/share/bash-completion/completions/{am,ai-mirror}
#   or copy to /etc/bash_completion.d/am
#
# Features:
#   - Dynamic subcommand and option completion
#   - Context-aware argument completion (projects, ai-users, files, dirs)
#   - Graceful degradation when am is not in PATH
#   - Debug mode: _AM_COMP_DEBUG=1 for troubleshooting

# ===========================================================================
#  Subcommand and option definitions (kept in sync with src/cli/parser.cpp)
# ===========================================================================

_am_subcommands="create mkdir cp mv touch cd list health force-destroy rm config status update watch"

_am_global_opts="-h --help -v --verbose"

# Per-subcommand options (key=subcommand, value=space-separated flags)
declare -A _am_cmd_opts=(
	[create]="-h --help"
	[mkdir]="-h --help"
	[cp]="-h --help -f --force"
	[mv]="-h --help -f --force"
	[touch]="-h --help"
	[cd]="-h --help"
	[list]="-h --help"
	[health]="-h --help"
	[force-destroy]="-h --help"
	[rm]="-h --help"
	[config]="-h --help"
	[status]="-h --help"
	[update]="-h --help"
	[watch]="-h --help"
)

# Per-subcommand: number of positional arguments expected
declare -A _am_cmd_nargs=(
	[create]=1
	[mkdir]=2
	[cp]=2
	[mv]=2
	[touch]=2
	[cd]=1
	[list]=0
	[health]=0
	[force-destroy]=1
	[rm]=1
	[config]=0
	[status]=0
	[update]=1
	[watch]=0
)

# ===========================================================================
#  Helper: debug logging
# ===========================================================================

_am_debug() {
	if [[ ${_AM_COMP_DEBUG:-0} == "1" ]]; then
		local file="${_AM_COMP_DEBUG_FILE:-/tmp/am-completion-debug.log}"
		echo "$@" >>"$file"
	fi
}

# ===========================================================================
#  Helper: resolve the am / ai-mirror binary
# ===========================================================================

_am_resolve_bin() {
	if command -v am &>/dev/null; then
		echo "am"
	elif command -v ai-mirror &>/dev/null; then
		echo "ai-mirror"
	else
		return 1
	fi
}

# ===========================================================================
#  Helper: get ai-user names (cached per completion session)
# ===========================================================================

_am_cached_users=""

_am_get_users() {
	if [[ -n "$_am_cached_users" ]]; then
		echo "$_am_cached_users"
		return
	fi

	local bin
	bin=$(_am_resolve_bin) || return

	_am_cached_users=$("$bin" list 2>/dev/null | command grep -E '^  [a-zA-Z0-9_]' | command awk '{print $1}')
	echo "$_am_cached_users"
}

# ===========================================================================
#  Helper: get project base directories from config
#  Reads ~/.ai-mirror.toml [user].allowed_bases and adds $HOME
# ===========================================================================

_am_get_project_bases() {
	local config_file="${HOME}/.ai-mirror.toml"
	local bases=("$HOME")

	if [[ -r "$config_file" ]]; then
		# Extract allowed_bases entries from TOML
		# Handles multiline arrays: allowed_bases = [ "...", "...", ... ]
		local in_array=0
		while IFS= read -r line; do
			# Trim leading whitespace
			line="${line#"${line%%[![:space:]]*}"}"

			if [[ "$line" == "allowed_bases"*"="*"["* ]]; then
				in_array=1
				# Check if single-line array
				local content="${line#*\[}"
				content="${content%\]*}"
				if [[ -n "$content" && "$content" != *"]"* ]]; then
					_am_extract_paths "$content" bases
				fi
				if [[ "$line" == *"]"* ]]; then
					in_array=0
				fi
				continue
			fi

			if [[ $in_array -eq 1 ]]; then
				if [[ "$line" == *"]"* ]]; then
					in_array=0
					line="${line%\]*}"
				fi
				_am_extract_paths "$line" bases
			fi
		done <"$config_file"
	fi

	printf '%s\n' "${bases[@]}"
}

_am_extract_paths() {
	local content="$1"
	local -n _arr="$2"
	local path
	while [[ "$content" =~ \"([^\"]+)\" ]]; do
		path="${BASH_REMATCH[1]}"
		content="${content#*\"${BASH_REMATCH[1]}\"}"
		# Expand ~ and {user}
		path="${path//\{user\}/$(whoami)}"
		if [[ "$path" == "~/"* ]]; then
			path="${HOME}/${path#~/}"
		elif [[ "$path" == "~" ]]; then
			path="$HOME"
		fi
		if [[ -d "$path" ]]; then
			_arr+=("$path")
		fi
	done
}

# ===========================================================================
#  Helper: list immediate subdirectories of project bases
# ===========================================================================

_am_get_projects() {
	local cur="$1"
	local bases
	mapfile -t bases < <(_am_get_project_bases)

	local results=()
	for base in "${bases[@]}"; do
		local entries
		entries=$(command find "$base" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | command sort)
		while IFS= read -r entry; do
			[[ -z "$entry" ]] && continue
			# Offer both full path and basename
			results+=("$entry")
			local bn
			bn=$(basename "$entry")
			# Only add basename if it doesn't clash with a full path
			if [[ "$cur" != */* ]]; then
				results+=("$bn")
			fi
		done <<<"$entries"
	done

	# Deduplicate
	local deduped
	deduped=$(printf '%s\n' "${results[@]}" | command sort -u)
	echo "$deduped"
}

# ===========================================================================
#  Helper: complete with words (safe for unusual characters)
# ===========================================================================

_am_comp_words() {
	local words="$1"
	local cur="$2"
	local w
	while IFS= read -r w; do
		[[ -z "$w" ]] && continue
		COMPREPLY+=("$(compgen -W "$w" -- "$cur")")
	done <<<"$words"
}

# ===========================================================================
#  Helper: count positional args (non-option words after subcommand)
# ===========================================================================

_am_count_positionals() {
	local -n _words="${1:-COMP_WORDS}"
	local cmd_idx="$2"
	local count=0
	local i
	for ((i = cmd_idx + 1; i < ${#_words[@]}; i++)); do
		if [[ "${_words[$i]}" != -* ]]; then
			((count++))
		fi
	done
	echo "$count"
}

# ===========================================================================
#  Main completion function
# ===========================================================================

_am_completion() {
	local cur prev words cword
	_init_completion -n = || return

	local cmd=""
	local cmd_idx=-1
	local i

	_am_debug "=== _am_completion: COMP_WORDS=(${COMP_WORDS[*]}) cword=$cword cur='$cur' ==="

	# Find the subcommand (first non-option word at index >= 1)
	for ((i = 1; i < ${#words[@]}; i++)); do
		if [[ "${words[$i]}" != -* ]]; then
			cmd="${words[$i]}"
			cmd_idx=$i
			break
		fi
	done

	_am_debug "cmd='$cmd' cmd_idx=$cmd_idx"

	# -----------------------------------------------------------------------
	#  Level 0: completing the subcommand itself (or global options)
	# -----------------------------------------------------------------------
	if [[ -z "$cmd" || $cword -le $cmd_idx ]]; then
		COMPREPLY+=($(compgen -W "$_am_subcommands" -- "$cur"))
		COMPREPLY+=($(compgen -W "$_am_global_opts" -- "$cur"))
		return
	fi

	# -----------------------------------------------------------------------
	#  Level 1: inside a subcommand — complete options
	# -----------------------------------------------------------------------
	if [[ "$cur" == -* ]]; then
		local opts="${_am_cmd_opts[$cmd]:--h --help}"
		COMPREPLY+=($(compgen -W "$opts" -- "$cur"))
		return
	fi

	# -----------------------------------------------------------------------
	#  Level 2: positional argument completion (context-aware)
	# -----------------------------------------------------------------------
	local n_positionals
	n_positionals=$(_am_count_positionals COMP_WORDS "$cmd_idx")
	_am_debug "cmd=$cmd n_positionals=$n_positionals"

	case "$cmd" in
	create)
		# create <project_path>
		if [[ $n_positionals -le 1 ]]; then
			local projects
			projects=$(_am_get_projects "$cur")
			_am_comp_words "$projects" "$cur"
			_filedir -d
		fi
		;;

	mkdir)
		# mkdir <path> <ai_user>
		if [[ $n_positionals -eq 1 ]]; then
			_filedir -d
		elif [[ $n_positionals -eq 2 ]]; then
			local users
			users=$(_am_get_users)
			_am_comp_words "$users" "$cur"
		fi
		;;

	touch)
		# touch <path> <ai_user>
		if [[ $n_positionals -eq 1 ]]; then
			_filedir
		elif [[ $n_positionals -eq 2 ]]; then
			local users
			users=$(_am_get_users)
			_am_comp_words "$users" "$cur"
		fi
		;;

	cp | mv)
		# cp/mv <src> <dst>
		_filedir
		;;

	cd)
		# cd <path>
		_filedir -d
		;;

	rm | update)
		# rm/update <project_path>
		if [[ $n_positionals -le 1 ]]; then
			local projects users
			projects=$(_am_get_projects "$cur")
			users=$(_am_get_users)
			_am_comp_words "$projects" "$cur"
			_am_comp_words "$users" "$cur"
			_filedir -d
		fi
		;;

	force-destroy)
		# force-destroy <username_or_path>
		if [[ $n_positionals -le 1 ]]; then
			local users projects
			users=$(_am_get_users)
			projects=$(_am_get_projects "$cur")
			_am_comp_words "$users" "$cur"
			_am_comp_words "$projects" "$cur"
			_filedir -d
		fi
		;;

	status)
		# status [optional project/user]
		if [[ $n_positionals -le 1 ]]; then
			local users projects
			users=$(_am_get_users)
			projects=$(_am_get_projects "$cur")
			_am_comp_words "$users" "$cur"
			_am_comp_words "$projects" "$cur"
			_filedir -d
		fi
		;;

	list | config | health | watch)
		# No positional arguments
		;;

	*)
		# Unknown subcommand — fallback to file completion
		_filedir
		;;
	esac

	_am_debug "COMPREPLY=(${COMPREPLY[*]})"
}

# ===========================================================================
#  Register for both command names
# ===========================================================================

complete -F _am_completion am
complete -F _am_completion ai-mirror

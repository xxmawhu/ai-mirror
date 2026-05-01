# bash completion for am (ai-mirror)
# Install: source this file or place in /etc/bash_completion.d/ or ~/.local/share/bash-completion/completions/
#
# Completion provides:
# - Subcommands: create, mkdir, cp, mv, touch, cd, list, health, force-destroy, rm, config, status, update, watch
# - Global options: -h, --help, -v, --verbose
# - Context-aware arguments: project paths, ai-user names, files/dirs

_am_comp_subcommands="create mkdir cp mv touch cd list health force-destroy rm config status update watch"
_am_comp_global_opts="-h --help -v --verbose"

# Get list of managed ai-users for current user
_am_comp_get_users() {
	local users
	users=$(ai-mirror list 2>/dev/null | grep -E '^  [a-zA-Z0-9_]' | awk '{print $1}')
	COMPREPLY+=($(compgen -W "$users" -- "$cur"))
}

# Get project directories under HOME/projects for current user
_am_comp_get_projects() {
	local projects
	projects=$(find ~/projects -maxdepth 1 -type d 2>/dev/null | tail -n +2 | xargs -I{} basename {})
	COMPREPLY+=($(compgen -W "$projects" -- "$cur"))
}

# Main completion function
_am_completion() {
	local cur prev words cword
	_init_completion || return

	local cmd="${words[1]}"

	# If no command yet, complete subcommands and global options
	if [[ $cword -eq 1 ]]; then
		COMPREPLY+=($(compgen -W "$_am_comp_subcommands" -- "$cur"))
		COMPREPLY+=($(compgen -W "$_am_comp_global_opts" -- "$cur"))
		return
	fi

	# Handle options after subcommand
	if [[ "$cur" == -* ]]; then
		case "$cmd" in
		create | mkdir | touch | cd | rm | force-destroy | update | status | health)
			COMPREPLY+=($(compgen -W "-h --help" -- "$cur"))
			;;
		cp | mv)
			COMPREPLY+=($(compgen -W "-h --help -f --force" -- "$cur"))
			;;
		list | config | watch)
			COMPREPLY+=($(compgen -W "-h --help" -- "$cur"))
			;;
		esac
		return
	fi

	# Handle arguments based on subcommand
	case "$cmd" in
	create)
		# create <project_path>
		if [[ $cword -eq 2 ]]; then
			_am_comp_get_projects
			_filedir -d
		fi
		;;
	cd)
		# cd <path>
		_filedir -d
		;;
	mkdir)
		# mkdir <path>
		if [[ $cword -eq 2 ]]; then
			_am_comp_get_projects
			_filedir -d
		fi
		;;
	touch)
		# touch <path>
		_filedir
		;;
	cp | mv)
		# cp/mv <src> <dst>
		_filedir
		;;
	rm)
		# rm <project_path>
		if [[ $cword -eq 2 ]]; then
			_am_comp_get_projects
			_am_comp_get_users
			_filedir -d
		fi
		;;
	force-destroy)
		# force-destroy <username_or_path>
		if [[ $cword -eq 2 ]]; then
			_am_comp_get_users
			_am_comp_get_projects
			_filedir -d
		fi
		;;
	update)
		# update <project_path>
		if [[ $cword -eq 2 ]]; then
			_am_comp_get_projects
			_am_comp_get_users
			_filedir -d
		fi
		;;
	status)
		# status [optional: no args or project]
		if [[ $cword -eq 2 ]]; then
			_am_comp_get_users
			_filedir -d
		fi
		;;
	health)
		# health [optional: no args needed]
		;;
	list | config | watch)
		# These commands take no arguments
		;;
	*)
		# Unknown command, fall back to file completion
		_filedir
		;;
	esac
}

# Register completion function
complete -F _am_completion am
complete -F _am_completion ai-mirror

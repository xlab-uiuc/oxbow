#!/bin/bash
# Script to send signals for profiling.
set -e

print_usage() {
	echo "Usage: $(basename "$0") -s <reset|print> -t <devfs|secure_daemon|other_process>"
}

TARGET=""
VAL=""
SIGNO="SIGRTMIN" # Set proper signal set by setup_pf_sig_handlers().

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then # script is executed directly.

	while getopts "s:t:?h" opt; do
		case $opt in
		s)
			if [ "$OPTARG" = "reset" ]; then
				VAL=1
			elif [ "$OPTARG" = "print" ]; then
				VAL=2
			else
				print_usage
				exit 2
			fi
			;;
		t)
			TARGET=$OPTARG
			;;
		h | ?)
			print_usage
			exit 2
			;;
		esac
	done

	if [ -z "$TARGET" ] || [ -z "$VAL" ]; then
		print_usage
		exit 2
	fi

	cmd="sudo pkill -${SIGNO} -q ${VAL} $TARGET"
	echo $cmd
	$cmd
fi

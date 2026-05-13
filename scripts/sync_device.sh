#!/bin/bash
# Script to send signals to synchronize contents between two different storage devices.
# This script can be used to test checkpointing in qemu with two different NVME devices
# Cf. USE_NVME_STORAGE_ENGINE.

set -e

print_usage() {
	echo "Usage: $(basename "$0") -s <stage|fs> -t <devfs|secure_daemon>"
}

TARGET=""
VAL=""
SIGNO="SIGRTMIN" # Set proper signal set by setup_pf_sig_handlers().

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then # script is executed directly.

	while getopts "s:t:?h" opt; do
		case $opt in
		s)
			if [ "$OPTARG" = "stage" ]; then
				VAL=1
			elif [ "$OPTARG" = "fs" ]; then
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

	cmd="sudo pkill -$(expr $(kill -l SIGRTMIN) + 2) -q ${VAL} $TARGET"
	echo $cmd
	$cmd
fi

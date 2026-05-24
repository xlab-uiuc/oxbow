#!/bin/bash
set -xe

source ${SECURE_DAEMON}/secure_daemon_conf.sh

REMOTE_HOST=${rpc_rdma_ip_addr}
while getopts "l" opt; do
	case ${opt} in
		l)
			REMOTE_HOST=localhost
			;;
		*)
			echo "Usage: $0 [-l]"
			exit 1
			;;
	esac
done
sig_nu=$(($(kill -l SIGRTMIN) + 1))
cmd="sudo pkill -${sig_nu} devfs"

echo $cmd

ssh "${REMOTE_HOST}" "$cmd"

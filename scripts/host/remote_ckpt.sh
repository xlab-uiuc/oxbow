#!/bin/bash
set -xe

source ${SECURE_DAEMON}/secure_daemon_conf.sh

REMOTE_HOST=${rpc_rdma_ip_addr}
sig_nu=$(expr $(kill -l SIGRTMIN) + 1)
cmd="sudo pkill -${sig_nu} devfs"

echo $cmd

ssh ${REMOTE_HOST} $cmd

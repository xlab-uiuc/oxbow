#!/bin/bash
# Make sure that the proper configs are set in the secure_daemon_conf.sh and devfs_conf.sh.
set -e

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

if [ "$1" == "host" ]; then
	$SCRIPTS/host/setup_sriov_vf.sh && \
	$SCRIPTS/host/setup_spdk_host-journaling.sh
	echo "Setup Done."
else
	$SCRIPTS/host/nvme-of/config_nvmf_server.sh && \
	$SCRIPTS/host/nvme-of/config_nvmf_client.sh && \
	$SCRIPTS/host/setup_sriov_vf.sh && \
	$SCRIPTS/host/setup_spdk.sh && \
	echo "Setup Done."
fi



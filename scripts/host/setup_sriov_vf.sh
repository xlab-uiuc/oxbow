#!/bin/bash
# Helper script to setup Virtual Function (VF) of SSD that supports SR-IOV
# (PM1735) for Oxbow.
set -e

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi
NVME_BIN=$TOOLS/bin/nvme "$SCRIPTS/host/nvme-sriov/setup_vf.sh"

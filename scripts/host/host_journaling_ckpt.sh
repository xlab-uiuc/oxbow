#!/bin/bash
# Do checkpointing with host journaling.
#

sudo rm -rf /tmp/fs_area.dump
sudo rm -rf /tmp/stage_area.dump

# 1. Copy the stage area to the devfs's device.
$OXBOW_ROOT/scripts/sync_device.sh -s stage -t secure_daemon

echo "Wait until Secure Daemon finishes dumping the stage area, then press enter."
read
$OXBOW_ROOT/scripts/sync_device.sh -s stage -t devfs

# 2. Do checkpointing.
echo "Press enter to start checkpointing."
read
$OXBOW_ROOT/scripts/device/send_ckpt_signal.sh

# 3. Copy the file system area to the secure daemon's device.
echo "Wait until DevFS finishes checkpointing, then press enter to dump DevFS's file system area."
read
$OXBOW_ROOT/scripts/sync_device.sh -s fs -t devfs

echo "Wait until DevFS finishes dumping the file system area, then press enter."
read
$OXBOW_ROOT/scripts/sync_device.sh -s fs -t secure_daemon

echo "Checkpoint (host journaling mode) completed."

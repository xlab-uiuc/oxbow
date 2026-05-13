#!/bin/bash
set -xe

sudo pkill -$(expr $(kill -l SIGRTMIN) + 1) devfs

exit

# Get the PID of the devfs program
pid=$(pgrep devfs)

if [ -z "$pid" ]; then
    echo "DevFS process not found"
    exit 1
fi

# Send SIGRTMIN+1 to the process
sudo kill -$(expr $(kill -l SIGRTMIN) + 1) $pid

echo "Checkpoint signal sent to DevFS (PID: $pid)"

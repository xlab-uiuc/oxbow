#!/bin/bash
set -euo pipefail

SESSION_NAME="${SESSION_NAME:-oxbow-exp}"
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if ! command -v tmux >/dev/null 2>&1; then
	echo "tmux is required."
	exit 1
fi

if ! tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
	tmux new-session -d -s "$SESSION_NAME" -c "$SCRIPT_DIR"
	secure_pane=$(tmux display-message -p -t "$SESSION_NAME":0.0 '#{pane_id}')

	tmux send-keys -t "$secure_pane" \
		"source set_env.sh && sudo mkdir -p \"\$OXBOW_PREFIX\" && cd oxbow/secure_daemon && clear && echo 'Waiting for DevFS RPC socket...' && until [ -S /tmp/oxbow_rpc_daemon_devfs ]; do sleep 1; done && ./run.sh" C-m

	devfs_pane=$(tmux split-window -h -t "$secure_pane" -c "$SCRIPT_DIR" -P -F '#{pane_id}')
	tmux resize-pane -t "$secure_pane" -R 80
	tmux send-keys -t "$devfs_pane" \
		"source set_env.sh && cd oxbow/devfs && ./run.sh" C-m

	bench_pane=$(tmux split-window -v -t "$devfs_pane" -c "$SCRIPT_DIR" -P -F '#{pane_id}')
	tmux send-keys -t "$bench_pane" \
		"source set_env.sh && cd oxbow/libfs && clear && echo 'Benchmark pane. DevFS and Secure Daemon are starting in the other panes.'" C-m

	shell_pane=$(tmux split-window -v -t "$secure_pane" -c "$SCRIPT_DIR" -P -F '#{pane_id}')
	tmux send-keys -t "$shell_pane" \
		"source set_env.sh && sudo dmesg --follow" C-m

	tmux select-pane -t "$bench_pane"
fi

tmux attach -t "$SESSION_NAME"

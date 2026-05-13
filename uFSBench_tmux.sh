#!/bin/bash

SESSION_NAME="oxbow-exp"

sudo mount -t tmpfs tmpfs /tmp

if ! tmux has-session -t $SESSION_NAME 2>/dev/null; then
  # Setup the machine.
  source set_env.sh && scripts/host/setup_machine_all.sh
  source set_env.sh # Re-source it as nvme dev name can change during initialization.

  # Create a new session.
  tmux new-session -d -s $SESSION_NAME

  sleep 0.2

  tmux send-keys -t "$SESSION_NAME":0.0 "source set_env.sh && cd oxbow/secure_daemon" C-m
  sleep 0.2

  tmux split-window -h -t "$SESSION_NAME":0.0
  tmux resize-pane -t "$SESSION_NAME":0.0 -R 80
  tmux select-pane -t "$SESSION_NAME":0.1
  tmux send-keys -t "$SESSION_NAME":0.1 "source set_env.sh && sudo dmesg --follow" C-m
  sleep 0.2

  tmux select-pane -t "$SESSION_NAME":0.1
  tmux split-window -v
  tmux send-keys -t "$SESSION_NAME":0.2 "source set_env.sh && cd oxbow/devfs" C-m
  sleep 0.2

  tmux select-pane -t "$SESSION_NAME":0.0
  tmux split-window -v
  tmux send-keys -t "$SESSION_NAME":0.1 "source set_env.sh && cd bench/uFS" C-m
  sleep 0.2

fi

tmux attach -t $SESSION_NAME

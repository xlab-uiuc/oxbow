# Deploy Oxbow on the AE Testbed

This document is mainly intended for the OSDI 2026 Artifact Evaluation and focuses
on a specific testbed machine. However, it may also be helpful to users who want
to deploy Oxbow on a real machine. Here, we use automated scripts to make the AE
process easier.

## Prerequisites

We assume Oxbow has already been compiled and is ready to run. Refer to
[README.md](../README.md) for compilation instructions.

## Run Oxbow with scripts

Assuming the machine is in a clean state after rebooting, set up the machine and
launch the tmux session. You can also use these scripts just to create the tmux
session without changing the system state.

```shell
./uFSBench_tmux.sh
```

Note that the Oxbow experiment scripts assume the tmux pane layout created by
this script, so please do not change the existing pane layout. You may create as
many additional tmux windows as you want.

The tmux window contains four panes:

- Top-left: runs `secure_daemon`
- Bottom-left: runs benchmarks
- Top-right: available pane (for example, to run `mkfs` or check kernel messages with `dmesg`)
- Bottom-right: runs `devfs`

Next, format the file-system layout.

```shell
cd oxbow
./mkfs.sh
```

In the bottom-right pane, SSH into the BlueField-2 DPU and run `devfs`.

```shell
ssh libra06-bf2-rdma
cd /path/to/devfs
./run.sh devfs
```

If `devfs` starts successfully, you will see the following in the terminal.

```shell
   ██████╗ ██╗  ██╗██████╗  ██████╗ ██╗    ██╗███████╗███████╗
  ██╔═══██╗╚██╗██╔╝██╔══██╗██╔═══██╗██║    ██║██╔════╝██╔════╝
  ██║   ██║ ╚███╔╝ ██████╔╝██║   ██║██║ █╗ ██║█████╗  ███████╗
  ██║   ██║ ██╔██╗ ██╔══██╗██║   ██║██║███╗██║██╔══╝  ╚════██║
  ╚██████╔╝██╔╝ ██╗██████╔╝╚██████╔╝╚███╔███╔╝██║     ███████║
   ╚═════╝ ╚═╝  ╚═╝╚═════╝  ╚═════╝  ╚══╝╚══╝ ╚═╝     ╚══════╝
┌───────────┐      ┌─────────────────────┐      ╔═══════════════╗
│   LibFS   │<────>│    Secure Daemon    │<────>║ >>  DevFS <<  ║
│           │      │                     │      ║   ACTIVATED   ║
└───────────┘      └─────────────────────┘      ╚═══════════════╝
```

Note that after resetting the DPU, you need to run
`scripts/device/setup_spdk.sh` on the device.

In the top-left tmux pane, run `secure_daemon`.

```shell
sudo umount /oxbow # Make sure that it is not mounted.
./run.sh
```

If `secure_daemon` runs correctly, you will see the following message.

```shell
   ██████╗ ██╗  ██╗██████╗  ██████╗ ██╗    ██╗███████╗███████╗
  ██╔═══██╗╚██╗██╔╝██╔══██╗██╔═══██╗██║    ██║██╔════╝██╔════╝
  ██║   ██║ ╚███╔╝ ██████╔╝██║   ██║██║ █╗ ██║█████╗  ███████╗
  ██║   ██║ ██╔██╗ ██╔══██╗██║   ██║██║███╗██║██╔══╝  ╚════██║
  ╚██████╔╝██╔╝ ██╗██████╔╝╚██████╔╝╚███╔███╔╝██║     ███████║
   ╚═════╝ ╚═╝  ╚═╝╚═════╝  ╚═════╝  ╚══╝╚══╝ ╚═╝     ╚══════╝
┌───────────┐      ╔═════════════════════╗      ┌───────────────┐
│   LibFS   │<────>║ >> Secure Daemon << ║<────>│     DevFS     │
│           │      ║      ACTIVATED      ║      │   ACTIVATED   │
└───────────┘      ╚═════════════════════╝      └───────────────┘
```

At this point, you are ready to use the Oxbow file system.

## Run simple test

For a simple test, we recommend running Oxbow's microbenchmark (`bench/micro`),
which is different from the one used in the paper. Please refer to
[bench.md](bench.md#microbenchmark) for instructions on running the microbenchmark.

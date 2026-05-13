# Deploy Oxbow on the AE Testbed

This document is mainly intended for OSDI 2026 Artifact Evaluation and focuses
on a specific testbed machine. However, it may also be helpful to users who want
to deploy Oxbow on a real machine. Here, we use automated scripts to make the AE
process easier.

## Prerequisites

We assume Oxbow has already been compiled and runnable. Refer to
[README.md](../README.md) for compilation instructions.

## Run Oxbow with scripts

Assuming that the machine is in a clean state after rebooting, set up the
machine and launch Tmux sessions. You can also use these scripts just to create
Tmux sessions without causing any side effects.

```shell
./uFSBench_tmux.sh
```

Note that Oxbow experiment scripts assume the current Tmux pane layout, so
please do not change the existing pane layout. You may create as many additional
Tmux windows as you want.

You will see four sessions. Each session is for:

- Left top: running secure_daemon
- Left bottom: running benchmarks
- Right top: free session (You can do mkfs or check kernel messages using `dmesg`)
- Right bottom: running devfs

Next, format the file system layout.

```shell
cd oxbow
./mkfs.sh
```

In the right bottom session, SSH into the BlueField-2 DPU and run devfs.

```shell
ssh libra06-bf2-rdma
cd /path/to/devfs
./run.sh devfs
```

If devfs is successfully executed, you will see the following in the terminal.

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

In the left top Tmux session, run secure_daemon.

```shell
sudo umount /oxbow # Make sure that it is not mounted.
./run.sh
```

If secure_daemon runs correctly, you will see the following message.

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

For simple test, we recommend running Oxbow's microbenchmark (`bench/micro`),
which is different from the one used in the paper. Please refer to
[bench.md](bench.md#microbenchmark) for instructions on running the microbenchmark.

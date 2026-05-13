# Profiling Oxbow

## Latency breakdown

Set `OXBOW_PROFILE` in the `profile.h` file. You can store the result into
separate per-thread files.

```c
// Enable profile. Comment out to disable.
#define OXBOW_PROFILE
// If defined, the same contents are dumped to files in profile directory.
#define PF_PRINT_TO_FILE
```

To see the profile result, terminate the program or use a script:

```shell
scripts/send_profile_signal.sh -s print -t secure_daemon
```

Check the usage of the script.

## Threadpool queuing delay

Set `THPOOL_PROFILE` and `THPOOL_PROFILE_HIST` flag. It will show the queuing delay
information on termination.

For example, in `secure_daemon/meson.build`:

```meson
  c_args: ['-DLOG_USE_COLOR',
     '-DTHPOOL_PROFILE',
     '-DTHPOOL_PROFILE_HIST',
  ],

```


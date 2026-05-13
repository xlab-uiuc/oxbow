project(
  'oxbow_secure_daemon',
  'c',
  version: '0.1',
  # default_options: ['warning_level=3', 'buildtype=debug', 'b_sanitize=address'],
  # default_options: ['warning_level=3', 'buildtype=debug'],
  default_options: ['warning_level=3', 'buildtype=release'],
)

### default_options : ['buildtype=debug'])
### default_options : ['warning_level=3', 'buildtype=release'])

proj_root = meson.source_root()
build_root = meson.build_root()
current_dir = meson.current_source_dir()

# message('proj_root: ' + proj_root)
# message('build_root: ' + build_root)

cc = meson.get_compiler('c')

### Set file system.
### Currently supported file systems: 'sefs', 'lwext4'
# filesystem='sefs'
filesystem='lwext4'

### - *_inc : header files
### - *_lib : library
### - *_dep : dependencies (to library)

# add_global_arguments('-DFLAG=value', language : 'c')

## SPDK library. (Refer to https://spdk.io/doc/pkgconfig.html)
spdk_inc = include_directories('lib/spdk/build/include')
# Linking shared libraries.
spdk_nvme_dep = dependency('spdk_nvme', method: 'pkg-config', static: false)
spdk_env_dpdk_dep = dependency(
  'spdk_env_dpdk',
  method: 'pkg-config',
  static: false,
)
spdk_vmd_dep = dependency('spdk_vmd', method: 'pkg-config', static: false)
spdk_thread_dep = dependency('spdk_thread', method: 'pkg-config', static: false)

# Link libspdk.so instead of individual libraries. Refer to README file of the SPDK project.
# spdk_lib = 'spdk'
# spdk_lib_dir = current_dir + '/lib/spdk/build/lib'
# spdk_dep = declare_dependency(
#     link_args : ['-L' + spdk_lib_dir, '-l' + spdk_lib],
#     include_directories : spdk_inc)

# spdk_nvme_dep = dependency('spdk_nvme, spdk_env_dpdk, spdk_vmd', method : 'pkg-config', static : false)

# Linking static libraries.
# spdk_nvme_dep = dependency('spdk_nvme, spdk_env_dpdk, spdk_vmd, spdk_syslibs', method : 'pkg-config', static : true)
# spdk_nvme_dd =declare_dependency(dependencies : spdk_nvme_dep)

## RDMA libraries.
# rdma_inc = include_directories('lib/rdma-core/build/include')
# ibverbs_dep = dependency('libibverbs', method : 'pkg-config', static : false)
# rdmacm_dep = dependency('librdmacm', method : 'pkg-config', static : false)

## Logging library.
log_inc = include_directories('lib/log.c/src')

## c-thread-pool library.
thpool_inc = include_directories('lib/c-thread-pool')

## BitArray (bitmap) library.
bitarr_inc = include_directories('lib/BitArray')
bitarr_dir = proj_root + '/lib/BitArray'
bitarr_dep = declare_dependency(
  link_args: ['-L' + bitarr_dir, '-lbitarr'],
  include_directories: bitarr_inc,
)
# Or, add bitarr_lib instead of bitarr_dep
# bitarr_lib = cc.find_library('bitarr', dirs : [bitarr_dir])

## RPC library.
rpc_inc = include_directories('lib/oxbow-rpc/include')
rpc_dep = declare_dependency(
  dependencies: cc.find_library(
    'oxbow_rpc',
    dirs: [proj_root + '/lib/oxbow-rpc/build'],
  ),
  include_directories: rpc_inc,
)

## Data fetcher library.
df_inc = include_directories('lib/data_fetcher/include')
df_dep = declare_dependency(
  dependencies: cc.find_library(
    'data_fetcher',
    dirs: [proj_root + '/lib/data_fetcher/build'],
  ),
  include_directories: df_inc,
)

## Math library
m_dep = cc.find_library('m', required: false)

## Atomic library (needed for 16-byte atomics on some platforms)
atomic_dep = cc.find_library('atomic', required: false)

## Permission for shared memory
acl_dep = dependency('libacl', required : true)

## Secure daemon source code headers.
inc = include_directories('include')
common_inc = include_directories('include/common') # TODO: To be removed for clarity.
common_linux_inc = include_directories('include/common/linux') # TODO: To be removed for clarity.
fs_inc = include_directories('include/fs') # TODO: To be removed for clarity.
linux_inc = include_directories('include/linux') # TODO: To be removed for clarity.

secure_daemon_lib_inc = [
    inc,
    common_inc,
    log_inc,
    fs_inc,
    linux_inc,
    thpool_inc,
    spdk_inc,
    bitarr_inc,
  ]

secure_daemon_inc = [inc, common_inc, common_linux_inc, fs_inc]

mkfs_inc = [inc, common_inc, log_inc, spdk_inc, bitarr_inc, fs_inc]

## Source files.
secure_daemon_lib_srcs = [
  'lib/c-thread-pool/thpool.c',
  'lib/log.c/src/log.c',
  'src/global.c',
  'src/config.c',
  'src/secure_daemon.c',
  'src/msg.c',
  'src/libfs/msg_handler.c',
  'src/libfs/shm.c',
  'src/io_dispatcher.c',
  'src/io/nvme.c',
  'src/io/bio.c',
  'src/common/time_stat.c',
  'src/common/config.c',
  'src/common/shm.c',
  'src/fs/buffer.c',
  'src/fs/super.c',
  'src/fs/init.c',
  'src/fs/inode.c',
  'src/fs/journal.c',
  'src/fs/mpage.c',
  'src/fs/sync.c',
  'src/fs/posix.c',
  'src/kernfs/dir_ops.c',
  'src/kernfs/file_ops.c',
  'src/common/profile.c',
  'src/common/utils/exp_flag.c',
  'src/common/dirty_mgmt.c',
  'src/common/ring_buffer_mpmc.c',
  'src/common/oxb_slab.c',
  'src/utils/sync_device.c',
  'src/utils/print_tx.c',
  'src/utils/jlock_profile.c',
]

mkfs_srcs = ['lib/log.c/src/log.c']

if filesystem == 'sefs'
  fs_srcs = [
    'src/fs/sefs/inode.c',
    'src/fs/sefs/super.c',
    'src/fs/sefs/dir.c',
    'src/fs/sefs/bitmap.c',
  ]
  mkfs_srcs += ['src/fs/sefs/mkfs.c']

elif filesystem == 'lwext4'
  fs_srcs = [ # common.
    'src/fs/lwext4/ext4_balloc.c',
    'src/fs/lwext4/ext4_bitmap.c',
    'src/fs/lwext4/ext4_block_group.c',
    'src/fs/lwext4/ext4.c',
    'src/fs/lwext4/ext4_crc32.c',
    'src/fs/lwext4/ext4_debug.c',
    'src/fs/lwext4/ext4_dir.c',
    'src/fs/lwext4/ext4_dir_idx.c',
    'src/fs/lwext4/ext4_extent.c',
    'src/fs/lwext4/ext4_fs.c',
    'src/fs/lwext4/ext4_hash.c',
    'src/fs/lwext4/ext4_ialloc.c',
    'src/fs/lwext4/ext4_inode.c',
    # 'src/fs/lwext4/ext4_journal.c',
    'src/fs/lwext4/ext4_super.c',
    'src/fs/lwext4/ext4_trans.c',
    'src/fs/lwext4/ext4_xattr.c',
  ]
  mkfs_srcs += [ # mkfs only.
    'src/fs/lwext4/lwext4_mkfs.c',
    'src/fs/lwext4/ext4_mkfs.c',
    'src/fs/lwext4/blockdev/file_dev.c',
    'src/fs/lwext4/ext4_bcache_for_mkfs.c',
    'src/fs/lwext4/ext4_blockdev_for_mkfs.c',
   ]
  mkfs_srcs += fs_srcs

  # mkfs does not include the following files.
  fs_srcs += [
    'src/fs/lwext4/blockdev/blockdev.c',
    'src/fs/lwext4/ext4_bcache.c',
    'src/fs/lwext4/ext4_blockdev.c',
  ]

  secure_daemon_lib_inc += include_directories('include/fs/lwext4')
  secure_daemon_inc +=  include_directories('include/fs/lwext4') # TOCHECK: Do we need it?
  mkfs_inc += include_directories('include/fs/lwext4')
  mkfs_inc += secure_daemon_inc

else
  error('Unsupported file system: ' + filesystem)
endif

secure_daemon_lib_srcs += fs_srcs

# using SIMD operation
add_project_arguments('-mavx2', language: 'c')
add_project_arguments('-march=native', language: 'c')

secure_daemon_lib = shared_library(
  'oxbow_secure_daemon',
  secure_daemon_lib_srcs,
  # version : '1.1.1',
  # soversion : '0',
  include_directories: secure_daemon_lib_inc,
  dependencies: [
    spdk_nvme_dep,
    spdk_env_dpdk_dep,
    spdk_vmd_dep,
    spdk_thread_dep,
    rpc_dep,
    bitarr_dep,
    m_dep,
    df_dep,
    acl_dep,
    atomic_dep, # oxb_slab.c uses 16-byte atomic operations (tagged pointer with version)
  ],
  override_options: ['b_asneeded=false'],
  c_args: ['-DLOG_USE_COLOR',
    # '-DTHPOOL_PROFILE',
    # '-DTHPOOL_PROFILE_HIST',

    # IPC mode: per-inode msg_ring. read()/epoll if not set.
    # It should be matched with CONFIG_OXBOW_IPC_MSG_RING in kernel.
    # '-DOXBOW_IPC_MSG_RING',
  ],
  link_args: ['-lrt',
              '-lssl', # for spdk
              # '-lisal' # for spdk
            ],
)

secure_daemon_src = ['src/main.c']
secure_daemon_exe = executable(
  'secure_daemon',
  secure_daemon_src,
  include_directories: secure_daemon_inc,
  link_with: secure_daemon_lib,
  # override_options: ['b_sanitize=address'],
)

mkfs_exe = executable(
  'mkfs.' + filesystem,
  mkfs_srcs,
  include_directories: mkfs_inc,
  # dependencies: [spdk_nvme_dep, spdk_env_dpdk_dep, spdk_vmd_dep],
  link_with : secure_daemon_lib,
  override_options: ['b_asneeded=false', 'b_sanitize=none'],
  c_args: [
    '-DLOG_USE_COLOR',
    '-DMKFS' # For lwext4_mkfs.c
    ],
  # dependencies : [spdk_nvme_dd.as_link_whole()], # Static library
)

subdir('test')

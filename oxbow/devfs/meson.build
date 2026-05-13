project('oxbow_devfs', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'buildtype=release'])
  # default_options : ['warning_level=3', 'buildtype=debug'])
  # default_options : ['warning_level=3', 'buildtype=debug', 'b_sanitize=address'])

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
spdk_nvme_dep = dependency('spdk_nvme', method : 'pkg-config', static : false)
spdk_env_dpdk_dep = dependency('spdk_env_dpdk', method : 'pkg-config', static : false)
spdk_vmd_dep = dependency('spdk_vmd', method : 'pkg-config', static : false)

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

## Logging library.
log_inc = include_directories('lib/log.c/src')

## c-thread-pool library.
thpool_inc = include_directories('lib/c-thread-pool')

## BitArray (bitmap) library.
bitarr_inc = include_directories('lib/BitArray')
bitarr_dir = proj_root + '/lib/BitArray'
bitarr_dep = declare_dependency(
  link_args : ['-L' + bitarr_dir, '-lbitarr'],
  include_directories : bitarr_inc
  )

## RPC library.
rpc_inc = include_directories('lib/oxbow-rpc/include')
rpc_dep = declare_dependency(
  dependencies : cc.find_library('oxbow_rpc', dirs : [proj_root + '/lib/oxbow-rpc/build']),
  include_directories : rpc_inc)

## Data fetcher library.
df_inc = include_directories('lib/data_fetcher/include')
df_dep = declare_dependency(
  dependencies : cc.find_library('data_fetcher',
  dirs : [proj_root + '/lib/data_fetcher/build']),
  include_directories : df_inc)

## Math library
m_dep = cc.find_library('m', required : false)

## DevFS source code headers.
inc = include_directories('include')
common_inc = include_directories('include/common')
common_linux_inc = include_directories('include/common/linux')
common_utils_inc = include_directories('include/common/utils')

## Source files.
devfs_lib_srcs = [ \
'lib/c-thread-pool/thpool.c',
'lib/log.c/src/log.c',
'src/global.c',
'src/config.c',
'src/devfs.c',
'src/msg.c',
'src/msg_handler.c',
'src/common/storage_engine/storage_engine.c',
'src/common/storage_engine/se_nvme.c',
'src/common/storage_engine/se_nvmf_fast.c',
'src/common/time_stat.c',
'src/common/config.c',
'src/common/profile.c',
'src/common/utils/exp_flag.c',
'src/journal.c',
'src/utils/sync_device.c',
'src/utils/print_tx.c',
]

if filesystem == 'sefs'
  fs_srcs = ['src/fs/sefs.c']
elif filesystem == 'lwext4'
  fs_srcs = ['src/fs/lwext4.c']
else
  error('Unsupported file system: ' + filesystem)
endif

devfs_lib_srcs += fs_srcs

devfs_lib = shared_library('oxbow_devfs',
                          devfs_lib_srcs,
                          # version : '1.1.1',
                          # soversion : '0',
                          include_directories : [inc, common_inc,
                          common_linux_inc, common_utils_inc, log_inc,
                          thpool_inc, spdk_inc, rpc_inc ,bitarr_inc, df_inc],
                          dependencies : [spdk_nvme_dep, spdk_env_dpdk_dep,
                          spdk_vmd_dep, rpc_dep, bitarr_dep, m_dep, df_dep], # Shared library
                          override_options: ['b_asneeded=false'], # Required for SPDK shared library.
                          c_args: ['-DLOG_USE_COLOR'], # Print with color by log.c library.
                          link_args : ['-lrt',  # for time_stat
                          '-lssl', # for spdk
                          # '-lisal', # for spdk
                          ]
                          # dependencies : [spdk_nvme_dd.as_link_whole()], # Static library.
                          )
                            #  link_args : ['-Wl,-rpath=' + spdk_rpath_dir],

devfs_src = ['src/main.c']
devfs_exe = executable('devfs', devfs_src, include_directories : [inc,
common_inc, common_linux_inc, common_utils_inc], link_with : devfs_lib)

subdir('test')

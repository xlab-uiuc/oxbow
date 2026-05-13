project('oxbow_libfs', 'c',
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

### - *_inc : header files
### - *_lib : library
### - *_dep : dependencies (to library)


# add_global_arguments('-DFLAG=value', language : 'c')

## Syscall intercept library.
syscall_intercept_inc = include_directories('lib/syscall_intercept/include')
syscall_intercept_dep = dependency('libsyscall_intercept', method :
'pkg-config', static : false)
### Using find_library command.
# syscall_intercept_dep = declare_dependency(
#     dependencies : cc.find_library('libsyscall_intercept', dirs : [proj_root + '/lib/syscall_intercept/install/lib']),
#     include_directories : syscall_intercept_inc)

## SPDK library. (Refer to https://spdk.io/doc/pkgconfig.html)
#spdk_inc = include_directories('lib/spdk/build/include')
# Linking shared libraries.
#spdk_nvme_dep = dependency('spdk_nvme', method : 'pkg-config', static : false)
#spdk_env_dpdk_dep = dependency('spdk_env_dpdk', method : 'pkg-config', static : false)
#spdk_vmd_dep = dependency('spdk_vmd', method : 'pkg-config', static : false)


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
thread_dep = dependency('threads') # -pthread

## Math library
m_dep = cc.find_library('m', required: false)

## BitArray (bitmap) library.
bitarr_inc = include_directories('lib/BitArray')
bitarr_dir = proj_root + '/lib/BitArray'
bitarr_dep = declare_dependency(
  link_args : ['-L' + bitarr_dir, '-lbitarr'],
  include_directories : bitarr_inc
  )
# Or, add bitarr_lib instead of bitarr_dep
# bitarr_lib = cc.find_library('bitarr', dirs : [bitarr_dir])

## RPC library.
rpc_inc = include_directories('lib/oxbow-rpc/include')
rpc_dep = declare_dependency(
  dependencies : cc.find_library('oxbow_rpc', dirs : [proj_root + '/lib/oxbow-rpc/build']),
  include_directories : rpc_inc)

## Libfs source code headers.
inc = include_directories('include')
common_inc = include_directories('include/common')
common_linux_inc = include_directories('include/common/linux')

## Source files.
libfs_srcs = [ \
'lib/c-thread-pool/thpool.c',
'lib/log.c/src/log.c',
'src/posix/posix_interface.c',
'src/shim/shim.c',
'src/global.c',
'src/config.c',
'src/filesystem/file.c',
'src/filesystem/fs.c',
'src/filesystem/read_write.c',
'src/filesystem/inode.c',
'src/filesystem/journal.c',
'src/msg/msg.c',
'src/msg/msg_handler.c',
'src/common/time_stat.c',
'src/common/shm.c',
#'src/block/bio.c',
'src/common/profile.c',
'src/common/dirty_mgmt.c',
]

oxbow_libfs_lib = shared_library('oxbow_libfs',
                        libfs_srcs,
                        # version : '1.1.1',
                        # soversion : '0',
                        include_directories : [inc, common_inc, common_linux_inc, thpool_inc,
                        log_inc, rpc_inc, syscall_intercept_inc, bitarr_inc],
						# spdk_inc],
                        dependencies : [syscall_intercept_dep, rpc_dep,
                        bitarr_dep, thread_dep, m_dep],
                        # dependencies : [thread_dep, m_dep],
						# spdk_nvme_dep, spdk_env_dpdk_dep, spdk_vmd_dep], # Shared library
                        # override_options: ['b_asneeded=false'], # Required for SPDK shared library.
                        c_args: [
                          '-DLOG_USE_COLOR',
                          '-DCHECK_PREFIX',
                          # '-DTHPOOL_PROFILE', # Profile thpool.
                          # '-DTHPOOL_PROFILE_HIST', # Histogram of thpool stats.
                          ], # Print with color by log.c library.
                        # dependencies : [spdk_nvme_dd.as_link_whole()], # Static library.
                        )
                            #  link_args : ['-Wl,-rpath=' + spdk_rpath_dir],

#mkfs_srcs = ['src/mkfs/mkfs.c', 'lib/log.c/src/log.c']
#mkfs_exe = executable('mkfs',
#                      mkfs_srcs,
#                      include_directories : [inc, common_inc, log_inc, spdk_inc],
#                      dependencies : [spdk_nvme_dep, spdk_env_dpdk_dep, spdk_vmd_dep], # Shared library
#                      override_options: ['b_asneeded=false'], # Required for SPDK shared library.
#                      c_args: ['-DLOG_USE_COLOR'], # Print with color by log.c library.
#                      # dependencies : [spdk_nvme_dd.as_link_whole()], # Static library
#                      )

subdir('test')

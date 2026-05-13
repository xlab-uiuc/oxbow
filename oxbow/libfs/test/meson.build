test_inc = include_directories('.')
srcs = [ \
'main.c',
'test_set.c',
'time_stat.c',
]
c_args = [
        # '-DCHECK_PREFIX'
        ]
## math.h dependency adding
m_dep = cc.find_library('m', required : false)

## Tests
main_test = executable('test_main', srcs,
                        include_directories : [inc, test_inc, log_inc],
                        link_with : oxbow_libfs_lib,
                        link_args: ['-lrt'], #real time latency
                        dependencies : [m_dep], #math library dependency
                        c_args : c_args
                        )

file_basic_test = executable('file_basic', 'file_basic.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        link_args: ['-lrt'], #real time latency
                        c_args : c_args
                        )
dir_test_exe = executable('dir_test', 'dir_test.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        c_args : c_args
                        )

dump_file_test_exe = executable('dump_file_test', 'dump_file_test.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        c_args : c_args
                        )

ls_test_exe = executable('ls_test', 'ls_test.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        c_args : c_args
                        )

spdk_test_exe = executable('spdk', 'spdk_test.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        c_args : c_args
                        )

fork_exec_test = executable('fork_exec', 'fork_exec_test.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        c_args : c_args
                        )

unaligned_write_exe = executable('unaligned_write', 'unaligned_write.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        link_args: ['-lrt'],
                        c_args : c_args
                        )

partial_rw_test_exe = executable('partial_rw_test', 'partial_rw_test.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        link_args: ['-lrt'],
                        c_args : c_args
                        )

random_read_verify_exe = executable('random_read_verify',
                        'random_read_verify.c',
                        include_directories : [inc, test_inc],
                        link_with : oxbow_libfs_lib,
                        link_args: ['-lrt'],
                        c_args : c_args
                        )

# journal_test = executable('journal_test', 'journal_test.c', include_directories : [inc, test_inc]
#                             , link_with : oxbow_libfs_lib
#                             , link_args: ['-lrt'] #real time latency
#                             )

# Do test.
# test('file_basic', file_basic_test, env : ['LD_PRELOAD=' + build_root + '/liboxbow_libfs.so'])
# test('dir_test', dir_test)
#test('spdk_test', spdk_test_exe, env : ['LD_PRELOAD=' + build_root + '/liboxbow_libfs.so'])
# test('fork_exec', fork_exec_test, env : ['LD_PRELOAD=' + build_root + '/liboxbow_libfs.so'])
test_inc = include_directories('.')

test_inc_dirs = [inc, common_inc, common_linux_inc, log_inc, test_inc,
bitarr_inc, fs_inc]

filesystems_inc = include_directories('../include/fs')
lwext4_inc = include_directories('../include/fs/lwext4')

## Set environment variables. 
# run_env = ['LD_LIBRARY_PATH=' + proj_root + '/lib/spdk/build/lib/:' + proj_root + '/lib/spdk/dpdk/build/lib/', 'LD_PRELOAD=' + build_root + '/liboxbow_devfs.so']
run_env = environment()
run_env.set('LD_LIBRARY_PATH', proj_root + '/lib/spdk/build/lib/:' + proj_root + '/lib/spdk/dpdk/build/lib/')

# Secure Daemon configurations. (Instead of sourcing secure_daemon_conf.sh directly. TODO: How to source secure_daemon_conf.sh?
run_env.set('rpc_port', '7174')
run_env.set('PCIE_NVME_ADDR', '') # ((WARN)) You must set proper PCIE address. Data can be lost.
# run_env.set('pcie_nvme_addr', '0000:d8:00.0') # ((WARN)) You must set proper PCIE address. Data can be lost.
run_env.set('rpc_rdma_thread_num', '1')
run_env.set('rpc_shmem_thread_num', '1')
run_env.set('storage_engine_thread_num', '1')
run_env.set('filesystem', 'sefs')

## Tests
example_test_exe = executable('example_test', 'example_test.c', include_directories : test_inc_dirs, link_with : secure_daemon_lib)
# bio_test_exe = executable('bio_test', 'bio_test.c', include_directories : test_inc_dirs, link_with : secure_daemon_lib)
iod_test_exe = executable('iod_test', 'iod_test.c', include_directories : test_inc_dirs, link_with : secure_daemon_lib)
dump_block_exe = executable('dump_block', 'dump_block.c', include_directories :
[test_inc_dirs, lwext4_inc, filesystems_inc], link_with : secure_daemon_lib)
nvme_direct_write_throughput_test_exe = executable('nvme_direct_write_throughput_test', 
                                             'nvme_direct_write_throughput_test.c', 
                                             include_directories :
                                             [test_inc_dirs, thpool_inc], 
                                             link_with : secure_daemon_lib)
nvme_bw_test_exe = executable('nvme_bw_test',
                              'nvme_bw_test.c',
                              include_directories : test_inc_dirs,
                              link_with : secure_daemon_lib)
nvme_mpage_read_throughput_test_exe = executable('nvme_mpage_read_throughput_test',
                              'nvme_mpage_read_throughput_test.c',
                              include_directories : test_inc_dirs,
                              link_with : secure_daemon_lib)

# Do test.
# test('example_test', example_test_exe, env : run_env, is_parallel : false)
# test('iod_test', iod_test_exe, env : run_env, is_parallel : false)
# test('bio_test', bio_test_exe, env : run_env, is_parallel : false)

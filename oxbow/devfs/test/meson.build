test_inc = include_directories('.')

test_inc_dirs = [inc, common_inc, common_linux_inc, log_inc, test_inc, thpool_inc]

## Set environment variables. 
# run_env = ['LD_LIBRARY_PATH=' + proj_root + '/lib/spdk/build/lib/:' + proj_root + '/lib/spdk/dpdk/build/lib/', 'LD_PRELOAD=' + build_root + '/liboxbow_devfs.so']
run_env = environment()
run_env.set('LD_LIBRARY_PATH', proj_root + '/lib/spdk/build/lib/:' + proj_root + '/lib/spdk/dpdk/build/lib/')
run_env.set('LD_PRELOAD',  build_root + '/liboxbow_devfs.so')

## DevFS configurations. (Instead of sourcing devfs_conf.sh directly. TODO: How to source devfs_conf.sh?
run_env.set('storage_engine_thread_num', '7')
run_env.set('rpc_rdma_thread_num', '1')

# ((WARN)) You must set proper values. Data can be lost.
run_env.set('rpc_rdma_port', '7174')
run_env.set('data_fetcher_rdma_port', '7175')
run_env.set('rpc_rdma_ip_addr', '')
run_env.set('pcie_nvme_addr', '')
run_env.set('nvmf_ip_addr', '')
run_env.set('nvmf_port', '4420')
run_env.set('nvmf_subnqn', 'oxbow-nvmf')

# Example configurations in Libra06.
# run_env.set('rpc_rdma_ip_addr', '192.168.14.113')
# run_env.set('pcie_nvme_addr', '0000:d8:00.0')
# run_env.set('nvmf_ip_addr', '192.168.14.113')


## Tests
# file_basic_test_exe = executable('file_basic', 'file_basic.c', include_directories : test_inc_dirs, link_with : devfs_lib)
# dir_test_exe = executable('dir_test', 'dir_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)
# spdk_test_exe = executable('spdk', 'spdk_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)
nvme_test_exe = executable('nvme_test', 'nvme_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)
nvmf_test_exe = executable('nvmf_test', 'nvmf_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)
# nvmf2_test_exe = executable('nvmf2_test', 'nvmf2_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)
storage_engine_test_exe = executable('storage_engine_test', 'storage_engine_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)
list_test_exe = executable('list_test', 'list_test.c', include_directories : test_inc_dirs)

# Add nvmf_throughput_test
nvmf_throughput_test_exe = executable('nvmf_throughput_test', 'nvmf_throughput_test.c', include_directories : test_inc_dirs, link_with : devfs_lib)

# Do test.
# test('nvme_test', nvme_test_exe, env : run_env, is_parallel : false)
# test('nvmf_test', nvmf_test_exe, env : run_env, is_parallel : false)
# test('nvme_storage_engine_test', storage_engine_test_exe, env : run_env, is_parallel : false, args : ['nvme'])
# test('nvmf_storage_engine_test', storage_engine_test_exe, env : run_env, is_parallel : false, args : ['nvmf'])
# test('nvmf_throughput_test', nvmf_throughput_test_exe, env : run_env, is_parallel : false)

# test('file_basic', file_basic_test, env : ['LD_PRELOAD=' + build_root + '/liboxbow_devfs.so'])
# test('dir_test', dir_test)
# test('spdk_test', spdk_test_exe, env : ['LD_PRELOAD=' + build_root + '/liboxbow_devfs.so'])

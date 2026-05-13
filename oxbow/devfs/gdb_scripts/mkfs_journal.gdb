#define load_sl
#        sharedlibrary libshim.so
#        sharedlibrary libmlfs.so
#end

set environment LD_LIBRARY_PATH ./lib/spdk/build/lib/:./lib/spdk/dpdk/build/lib/
#set auto-solib-add on
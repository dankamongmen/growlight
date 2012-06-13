#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <blkid/blkid.h>

#include <libblkid.h>
#include <growlight.h>

static blkid_cache cache;
static unsigned cache_once_success;
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

static void
init_blkid_cache(void){
	if(blkid_get_cache(&cache,NULL) == 0){
		if(blkid_probe_all(cache) == 0){
			blkid_gc_cache(cache);
			cache_once_success = 1;
		}
	}
}

// Call at the start of all entry points, pairing with blkid_exit on exit.
// Ensures the libblkid block cache is successfully initialized.
static int
blkid_entry(void){
	if(pthread_once(&cache_once,init_blkid_cache)){
		return -1;
	}
	if(!cache_once_success){
		return -1;
	}
	return 0;
}

static inline int
blkid_exit(int ret){
	return ret;
}

int close_blkid(void){
	if(blkid_entry()){
		return -1;
	}
	blkid_put_cache(cache);
	return blkid_exit(0);
}

int probe_blkid_dev(const char *dev,blkid_probe *pr){
	if(blkid_entry()){
		return -1;
	}
	if((*pr = blkid_new_probe_from_filename(dev)) == NULL){
		return blkid_exit(-1);
	}
	if(blkid_probe_enable_topology(*pr,1)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_enable_superblocks(*pr,1)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_set_superblocks_flags(*pr,~0u)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_enable_partitions(*pr,1)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_set_partitions_flags(*pr,BLKID_PARTS_ENTRY_DETAILS)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_do_fullprobe(*pr)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_exit(0)){
		blkid_free_probe(*pr);
		return -1;
	}
	return 0;
}

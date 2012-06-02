#include <pthread.h>
#include <blkid/blkid.h>

#include <libblkid.h>
#include <growlight.h>

static blkid_cache cache;
static unsigned cache_once_success;
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void
init_blkid_cache(void){
	if(blkid_get_cache(&cache,NULL) == 0){
		if(blkid_probe_all(cache) == 0){
			blkid_gc_cache(cache);
			cache_once_success = 1;
		}
	}
}

// Call at the start of all entry points. Ensures the libblkid block cache is
// successfully initialized upon first call, and takes the lock.
static int
blkid_entry(void){
	if(pthread_once(&cache_once,init_blkid_cache)){
		return -1;
	}
	if(!cache_once_success){
		return -1;
	}
	if(pthread_mutex_lock(&cache_lock)){
		return -1;
	}
	return 0;
}

static inline int
blkid_exit(void){
	return pthread_mutex_unlock(&cache_lock);
}

int load_blkid_superblocks(void){
	blkid_dev_iterate biter;
	blkid_dev dev;
	int r;

	if(blkid_entry()){
		return -1;
	}
	if((biter = blkid_dev_iterate_begin(cache)) == NULL){
		goto err;
	}
	r = 0;
	while(blkid_dev_next(biter,&dev) == 0){
		const char *name;

		if((name = blkid_dev_devname(dev)) == NULL){
			r = -1;
		}else if(lookup_device(name) == NULL){
			r = -1;
		}
	}
	blkid_dev_iterate_end(biter);
	if(r){
		goto err;
	}
	return blkid_exit();

err:
	blkid_exit();
	return -1;
}

int close_blkid(void){
	if(blkid_entry()){
		return -1;
	}
	blkid_put_cache(cache);
	return blkid_exit();
}

int probe_blkid_dev(const char *dev,blkid_probe *pr){
	if(blkid_entry()){
		return -1;
	}
	if((*pr = blkid_new_probe_from_filename(dev)) == NULL){
		goto err;
	}
	if(blkid_probe_enable_topology(*pr,1)){
		goto err;
	}
	if(blkid_probe_enable_superblocks(*pr,1)){
		goto err;
	}
	if(blkid_probe_enable_partitions(*pr,1)){
		goto err;
	}
	return blkid_exit();

err:
	blkid_exit();
	return -1;
}

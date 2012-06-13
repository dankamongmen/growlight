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
		fprintf(stderr,"Couldn't enable blkid superprobe for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_set_superblocks_flags(*pr,~0u)){
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_enable_partitions(*pr,1)){
		fprintf(stderr,"Couldn't enable blkid partitionprobe for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(*pr);
		return blkid_exit(-1);
	}
	if(blkid_probe_set_partitions_flags(*pr,BLKID_PARTS_ENTRY_DETAILS)){
		fprintf(stderr,"Couldn't set blkid partitionflags for %s (%s?)\n",dev,strerror(errno));
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

/*int crap_blk_probe(void){
	int n;
	size_t len;
	const char *val,*name;

	blkid_probe bp = blkid_new_probe_from_filename("/dev/sde1");
	blkid_probe_enable_partitions(bp,1);
	blkid_probe_set_partitions_flags(bp,BLKID_PARTS_ENTRY_DETAILS);
	blkid_probe_enable_superblocks(bp,BLKID_SUBLKS_DEFAULT | BLKID_SUBLKS_VERSION);
	blkid_do_fullprobe(bp);
	n = blkid_probe_numof_values(bp);
	while(n--){
		blkid_probe_get_value(bp,n,&name,&val,&len);
		printf("%s: %s\n",name,val);
	}
	return 0;
}*/

// Takes a /dev/ path, and examines the superblock therein for a valid
// filesystem or raid superblock.
int probe_blkid_superblock(const char *dev){
	const char *val,*name;
	char buf[PATH_MAX];
	blkid_probe bp;
	int n;
	size_t len;

	if(strncmp(dev,"/dev/",5)){
		if(snprintf(buf,sizeof(buf),"/dev/%s",dev) >= (int)sizeof(buf)){
			fprintf(stderr,"Bad name: %s\n",dev);
			return -1;
		}
		dev = buf;
	}
	if((bp = blkid_new_probe_from_filename(dev)) == NULL){
		fprintf(stderr,"Couldn't get blkid probe for %s (%s?)\n",dev,strerror(errno));
		return -1;
	}
	if(blkid_probe_enable_partitions(bp,1)){
		fprintf(stderr,"Couldn't enable blkid partitionprobe for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(bp);
		return -1;
	}
	if(blkid_probe_set_partitions_flags(bp,BLKID_PARTS_ENTRY_DETAILS)){
		fprintf(stderr,"Couldn't set blkid partitionflags for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(bp);
		return -1;
	}
	if(blkid_probe_enable_superblocks(bp,1)){
		fprintf(stderr,"Couldn't enable blkid superprobe for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(bp);
		return -1;
	}
	if(blkid_probe_set_superblocks_flags(bp,BLKID_SUBLKS_DEFAULT |
						BLKID_SUBLKS_VERSION)){
		fprintf(stderr,"Couldn't set blkid superflags for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(bp);
		return -1;
	}
	if(blkid_do_fullprobe(bp)){
		fprintf(stderr,"Couldn't run blkid fullprobe for %s (%s?)\n",dev,strerror(errno));
		blkid_free_probe(bp);
		return -1;
	}
	n = blkid_probe_numof_values(bp);
	while(n--){
		blkid_probe_get_value(bp,n,&name,&val,&len);
		printf("%s: %s\n",name,val);
	}
	blkid_free_probe(bp);
	return 0;
}

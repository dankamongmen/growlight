#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "zfs.h"
#include "config.h"
#include "growlight.h"

#ifdef HAVE_LIBZFS

// FIXME hacks around the libspl/libzfs autotools-dominated jank
#define HAVE_IOCTL_IN_SYS_IOCTL_H
#define ulong_t unsigned long
#define boolean_t bool
#include <stdbool.h>
#include <libzfs.h>

static libzfs_handle_t *zht;
static char history[HIS_MAX_RECORD_LEN];
// FIXME taken from zpool.c source
static char props[] = "name,size,allocated,free,capacity,dedupratio,health,altroot";

struct zpoolcb_t {
	unsigned pools;
};

static int
zpoolcb(zpool_handle_t *zhp,void *arg){
	assert(zhp && !arg);
	// FIXME
	return 0;
}

static int
scan_zpools(libzfs_handle_t *zfs){
	zprop_list_t *pools = NULL;
	struct zpoolcb_t cb;

	if(zprop_get_list(zfs,props,&pools,ZFS_TYPE_POOL)){
		fprintf(stderr,"Coudln't list ZFS pools\n");
		return -1;
	}
	// FIXME do what with it?
	zprop_free_list(pools);
	memset(&cb,0,sizeof(cb));
	if(zpool_iter(zht,zpoolcb,&cb)){
		fprintf(stderr,"Couldn't iterate over zpools\n");
		return -1;
	}
	verbf("Found %u ZFS zpool%s\n",cb.pools,cb.pools == 1 ? "" : "s");
	return 0;
}

int init_zfs_support(void){
	if((zht = libzfs_init()) == NULL){
		return -1;
	}
	libzfs_print_on_error(zht,true);
	zpool_set_history_str(PACKAGE, 0, NULL, history);
	if(zpool_stage_history(zht,history)){
		fprintf(stderr,"ZFS history didn't match!\n");
		return -1;
	}
	if(scan_zpools(zht)){
		stop_zfs_support();
		return -1;
	}
	verbf("Initialized ZFS support.\n");
	return 0;
}

int stop_zfs_support(void){
	if(zht){
		libzfs_fini(zht);
		zht = NULL;
	}
	return 0;
}

int print_zfs_version(FILE *fp){
	return fprintf(fp,"LLNL ZoL: ZPL version %s, SPA version %s\n",
			ZPL_VERSION_STRING,SPA_VERSION_STRING);
}
#else
int init_zfs_support(void){
	verbf("No ZFS support in this build.\n");
	return 0;
}

int stop_zfs_support(void){
	return 0;
}

int print_zfs_version(FILE *fp){
	return fprintf(fp,"No ZFS support in this build.\n");
}
#endif

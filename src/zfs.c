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

static uintmax_t
dehumanize(const char *num){
	unsigned long long ull,dec;
	char *e;

	ull = strtoul(num,&e,0);
	if(*e == '.'){
		dec = strtoul(++e,&e,0);
	}else{
		dec = 0;
	}
	switch(*e){
		case 'E': case 'e': ull = ull * 1000 + dec * 10; dec = 0;
		case 'P': case 'p': ull = ull * 1000 + dec * 10; dec = 0;
		case 'T': case 't': ull = ull * 1000 + dec * 10; dec = 0;
		case 'G': case 'g': ull = ull * 1000 + dec * 10; dec = 0;
		case 'M': case 'm': ull = ull * 1000 + dec * 10; dec = 0;
		case 'K': case 'k': ull = ull * 1000 + dec * 10; dec = 0;
			break;
		case '\0':
			if(dec){
				fprintf(stderr,"Couldn't convert dec: %s\n",num);
				ull = 0;
			}
			break;
		default:
			fprintf(stderr,"Couldn't convert number: %s\n",num);
			ull = 0;
	}
	return ull;
}

static int
zpoolcb(zpool_handle_t *zhp,void *arg){
	char size[10],guid[21],ashift[5];
	struct zpoolcb_t *cb = arg;
	uint64_t version;
	const char *name;
	nvlist_t *conf;
	device *d;
	int state;

	name = zpool_get_name(zhp);
	conf = zpool_get_config(zhp,NULL);
	if(!name || !conf){
		fprintf(stderr,"name/config failed for zpool\n");
		zpool_close(zhp);
		return -1;
	}
	if(strlen(name) >= sizeof(d->name)){
		fprintf(stderr,"zpool name too long: %s\n",name);
		zpool_close(zhp);
		return -1;
	}
	++cb->pools;
	if(nvlist_lookup_uint64(conf,ZPOOL_CONFIG_VERSION,&version)){
		fprintf(stderr,"Couldn't get %s zpool version\n",name);
		zpool_close(zhp);
		return -1;
	}
	if(version > SPA_VERSION){
		fprintf(stderr,"%s zpool version too new (%lu > %llu)\n",
				name,version,SPA_VERSION);
		zpool_close(zhp);
		return -1;
	}
	state = zpool_get_state(zhp);
	if(zpool_get_prop(zhp,ZPOOL_PROP_SIZE,size,sizeof(size),NULL)){
		fprintf(stderr,"Couldn't get size for %s\n",name);
		zpool_close(zhp);
		return -1;
	}
	if(zpool_get_prop(zhp,ZPOOL_PROP_ASHIFT,ashift,sizeof(ashift),NULL)){
		fprintf(stderr,"Couldn't get GUID for %s\n",name);
		zpool_close(zhp);
		return -1;
	}
	if(zpool_get_prop(zhp,ZPOOL_PROP_GUID,guid,sizeof(guid),NULL)){
		fprintf(stderr,"Couldn't get GUID for %s\n",name);
		zpool_close(zhp);
		return -1;
	}
	zpool_close(zhp);
	if((d = malloc(sizeof(*d))) == NULL){
		fprintf(stderr,"Couldn't allocate device (%s?)\n",strerror(errno));
		zpool_close(zhp);
		return -1;
	}
	memset(d,0,sizeof(*d));
	strcpy(d->name,name);
	d->model = strdup("LLNL ZoL");
	d->uuid = strdup(guid);
	d->layout = LAYOUT_ZPOOL;
	d->swapprio = SWAP_INVALID;
	d->size = dehumanize(size);
	if((d->physsec = (1u << dehumanize(ashift))) == 0){
		d->physsec = 512u;
	}
	d->zpool.state = state;
	d->zpool.transport = AGGREGATE_UNKNOWN;
	d->zpool.zpoolver = version;
	add_new_virtual_blockdev(d);
	return 0;
}

int scan_zpools(void){
	libzfs_handle_t *zfs = zht;
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
		fprintf(stderr,"Warning: couldn't initialize ZFS\n");
		return 0;
	}
	libzfs_print_on_error(zht,true);
	zpool_set_history_str(PACKAGE, 0, NULL, history);
	if(zpool_stage_history(zht,history)){
		fprintf(stderr,"ZFS history didn't match!\n");
		return -1;
	}
	if(scan_zpools()){
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

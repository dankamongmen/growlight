#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "zfs.h"
#include "popen.h"
#include "config.h"
#include "growlight.h"

#ifdef HAVE_LIBZFS

// FIXME hacks around the libspl/libzfs autotools-dominated jank
#define ulong_t unsigned long
#define boolean_t bool
#include <stdbool.h>
//#include <libzfs.h>

static libzfs_handle_t *zht;
static char history[HIS_MAX_RECORD_LEN];
// FIXME taken from zpool.c source
static char props[] = "name,size,allocated,free,capacity,dedupratio,health,altroot";

struct zpoolcb_t {
	unsigned pools;
	const glightui *gui;
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
				diag("Couldn't convert dec: %s\n",num);
				ull = 0;
			}
			break;
		default:
			diag("Couldn't convert number: %s\n",num);
			ull = 0;
	}
	return ull;
}

static int
zpoolcb(zpool_handle_t *zhp,void *arg){
	char size[10],guid[21],ashift[5],health[20];
	struct zpoolcb_t *cb = arg;
	uint64_t version;
	const char *name;
	nvlist_t *conf;
	device *d;
	int state;

	name = zpool_get_name(zhp);
	conf = zpool_get_config(zhp,NULL);
	if(!name || !conf){
		diag("name/config failed for zpool\n");
		zpool_close(zhp);
		return -1;
	}
	if(strlen(name) >= sizeof(d->name)){
		diag("zpool name too long: %s\n",name);
		zpool_close(zhp);
		return -1;
	}
	++cb->pools;
	if(nvlist_lookup_uint64(conf,ZPOOL_CONFIG_VERSION,&version)){
		diag("Couldn't get %s zpool version\n",name);
		zpool_close(zhp);
		return -1;
	}
	if(version > SPA_VERSION){
		diag("%s zpool version too new (%lu > %llu)\n",
				name,version,SPA_VERSION);
		zpool_close(zhp);
		return -1;
	}
	state = zpool_get_state(zhp);
	if(zpool_get_prop(zhp,ZPOOL_PROP_HEALTH,health,sizeof(health),NULL)){
		diag("Couldn't get health for %s\n",name);
		zpool_close(zhp);
		return -1;
	}
	if(strcmp(health,"FAULTED")){ // FIXME really?!?!
		if(zpool_get_prop(zhp,ZPOOL_PROP_SIZE,size,sizeof(size),NULL)){
			diag("Couldn't get size for zpool '%s'\n",name);
			zpool_close(zhp);
			return -1;
		}
		if(zpool_get_prop(zhp,ZPOOL_PROP_ASHIFT,ashift,sizeof(ashift),NULL)){
			diag("Couldn't get ashift for zpool '%s'\n",name);
			zpool_close(zhp);
			return -1;
		}
	}else{
		verbf("Assuming zero size for faulted zpool '%s'\n",name);
		strcpy(ashift,"0");
		strcpy(size,"0");
	}
	if(zpool_get_prop(zhp,ZPOOL_PROP_GUID,guid,sizeof(guid),NULL)){
		diag("Couldn't get GUID for zpool '%s'\n",name);
		zpool_close(zhp);
		return -1;
	}
	if((d = malloc(sizeof(*d))) == NULL){
		diag("Couldn't allocate device (%s?)\n",strerror(errno));
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
	d->logsec = 512;
	d->zpool.state = state;
	d->zpool.transport = AGGREGATE_UNKNOWN;
	d->zpool.zpoolver = version;
	add_new_virtual_blockdev(d);
	zpool_close(zhp);
	return 0;
}

static int
zfscb(zfs_handle_t *zhf,void *arg){
	char sbuf[BUFSIZ],*mnttype,*label;
	struct zpoolcb_t *cb = arg;
	const glightui *gui;
	uintmax_t totalsize;
	const char *zname;
	zfs_type_t ztype;
	int version;
	device *d;

	gui = cb->gui;
	zname = zfs_get_name(zhf);
	ztype = zfs_get_type(zhf);
	if(!zname || !ztype){
		diag("Couldn't get ZFS name/type\n");
		return -1;
	}
	if((d = lookup_device(zname)) == NULL){
		diag("Coudln't look up zpool named %s\n",zname);
		return -1;
	}
	if((version = zfs_prop_get_int(zhf,ZFS_PROP_VERSION)) < 0){
		diag("Couldn't get dataset version for %s\n",zname);
		return -1;
	}
	totalsize = 0;
	if(zfs_prop_get(zhf,ZFS_PROP_AVAILABLE,sbuf,sizeof(sbuf),NULL,NULL,0,0)){
		diag("Couldn't get available size for %s\n",zname);
		return -1;
	}
	totalsize += dehumanize(sbuf);
	if(zfs_prop_get(zhf,ZFS_PROP_USED,sbuf,sizeof(sbuf),NULL,NULL,0,0)){
		diag("Couldn't get used size for %s\n",zname);
		return 0;
	}
	totalsize += dehumanize(sbuf);
	// ZFS_PROP_MOUNTPOINT is the default mount point, not the current
	// mount point (which indeed might not exist). No need to look it up.
	// FIXME check for existing mnttype?
	if((mnttype = strdup("zfs")) == NULL || (label = strdup(zname)) == NULL){
		diag("Couldn't dup string\n");
		free(mnttype);
		return -1;
	}
	free(d->label);
	free(d->mnttype);
	d->label = label;
	d->mnttype = mnttype;
	d->mntsize = totalsize;
	if(d->layout == LAYOUT_PARTITION){
		d = d->partdev.parent;
	}
	d->uistate = gui->block_event(d,d->uistate);
	return 0;
}

int scan_zpools(const glightui *gui){
	libzfs_handle_t *zfs = zht;
	zprop_list_t *pools = NULL;
	struct zpoolcb_t cb;

	if(zht == NULL){
		return 0; // ZFS wasn't successfully initialized
	}
	if(zprop_get_list(zfs,props,&pools,ZFS_TYPE_POOL)){
		diag("Coudln't list ZFS pools\n");
		return -1;
	}
	// FIXME do what with it?
	zprop_free_list(pools);
	memset(&cb,0,sizeof(cb));
	cb.gui = gui;
	if(zpool_iter(zht,zpoolcb,&cb)){
		diag("Couldn't iterate over zpools\n");
		return -1;
	}
	if(zfs_iter_root(zht,zfscb,&cb)){
		diag("Couldn't iterate over root datasets\n");
		return -1;
	}
	verbf("Found %u ZFS zpool%s\n",cb.pools,cb.pools == 1 ? "" : "s");
	return 0;
}

int init_zfs_support(const glightui *gui){
	if((zht = libzfs_init()) == NULL){
		diag("Warning: couldn't initialize ZFS\n");
		return 0;
	}
	libzfs_print_on_error(zht,verbose ? true : false);
	zpool_set_history_str(PACKAGE, 0, NULL, history);
	if(zpool_stage_history(zht,history)){
		diag("ZFS history didn't match!\n");
		return -1;
	}
	if(scan_zpools(gui)){
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

int destroy_zpool(device *d){
	if(d == NULL){
		diag("Passed a NULL zpool\n");
		return -1;
	}
	if(d->layout != LAYOUT_ZPOOL){
		diag("%s is not a zpool\n",d->name);
		return -1;
	}
	diag("Not yet implemented FIXME\n"); // FIXME
	return -1;
}

static int
generic_make_zpool(const char *type,const char *name,char * const *vdevs,int num){
	char buf[BUFSIZ];
	size_t left,pos;
	int z;

	pos = 0;
	left = sizeof(buf) - 1;
	for(z = 0 ; z < num ; ++z){
		if(strlen(vdevs[z]) >= left){
			diag("Too many arguments for zpool creation\n");
			return -1;
		}
		strcat(buf + pos,vdevs[z]);
		pos += strlen(vdevs[z]) + 1;
		buf[pos - 1] = ' ';
		left -= strlen(vdevs[z]) + 1;
	}
	buf[pos - 1] = '\0';
	return vspopen_drain("zpool create %s %s %s",type,name,buf);
}

int make_zmirror(const char *name,char * const *vdevs,int num){
	return generic_make_zpool("mirror",name,vdevs,num);
}

int make_raidz1(const char *name,char * const *vdevs,int num){
	return generic_make_zpool("raidz1",name,vdevs,num);
}

int make_raidz2(const char *name,char * const *vdevs,int num){
	return generic_make_zpool("raidz2",name,vdevs,num);
}

int make_raidz3(const char *name,char * const *vdevs,int num){
	return generic_make_zpool("raidz3",name,vdevs,num);
}

int make_zfs(const char *dev,const char *name){
	return vspopen_drain("zpool create %s %s",name,dev);
}
#else
int init_zfs_support(const glightui *gui __attribute__ ((unused))){
	diag("No ZFS support in this build.\n");
	return 0;
}

int stop_zfs_support(void){
	return 0;
}

int print_zfs_version(FILE *fp){
	return fprintf(fp,"No ZFS support in this build.\n");
}

int scan_zpools(const glightui *gui __attribute__ ((unused))){
	verbf("No ZFS support in this build.\n");
	return 0;
}

int destroy_zpool(device *d __attribute__ ((unused))){
	diag("No ZFS support in this build.\n");
	return 0;
}

int make_zfs(const char *dev __attribute__ ((unused)),
		const char *name __attribute__ ((unused))){
	diag("No ZFS support in this build.\n");
	return 0;
}
#endif

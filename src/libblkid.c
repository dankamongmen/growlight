#include <assert.h>
#include <wchar.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <blkid/blkid.h>

#include "fs.h"
#include "libblkid.h"
#include "growlight.h"

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

#include <unistd.h>
// Takes a /dev/ path, and examines the superblock therein for a valid
// filesystem or raid superblock.
int probe_blkid_superblock(const char *dev,blkid_probe *sbp,device *d){
	char buf[PATH_MAX],*mnttype,*uuid,*label,*partuuid;
	const char *val,*name;
	unsigned parttype;
	blkid_probe bp;
	wchar_t *pname;
	size_t len;
	int n;

	pname = NULL;
	parttype = 0;
	partuuid = uuid = label = mnttype = NULL;
	if(strncmp(dev,"/dev/",5)){
		if(snprintf(buf,sizeof(buf),"/dev/%s",dev) >= (int)sizeof(buf)){
			diag("Bad name: %s\n",dev);
			return -1;
		}
		dev = buf;
	}
	// This will sometimes fail due to the device node not yet existing. To
	// get here, however, we had to receive the name in a udev message, or
	// via discovery -- we've verified a /sys block entry. Go ahead and
	// loop a time or two, even though it's gross. I'd like a better way to
	// deal with this, obviously. FIXME
	{
		int i;

		for(i = 0 ; i < 3 ; ++i){
			if((bp = blkid_new_probe_from_filename(dev)) == NULL){
				if(errno == ENOMEDIUM){
					return -1;
				}
				diag("Couldn't get blkid probe for %s (%s?), retrying...\n",dev,strerror(errno));
				sleep(1);
			}else{
				break;
			}
		}
		if(bp == NULL){
			if((bp = blkid_new_probe_from_filename(dev)) == NULL){
				if(errno != ENOMEDIUM){
					diag("Couldn't get blkid probe for %s (%s?), retrying...\n",dev,strerror(errno));
					return -1;
				}
			}
		}
	}
	if(blkid_probe_enable_topology(bp,1)){
		diag("Couldn't enable blkid topology for %s (%s?)\n",dev,strerror(errno));
		return blkid_exit(-1);
	}
	if(blkid_probe_enable_partitions(bp,1)){
		diag("Couldn't enable blkid partitionprobe for %s (%s?)\n",dev,strerror(errno));
		goto err;
	}
	if(blkid_probe_set_partitions_flags(bp,BLKID_PARTS_ENTRY_DETAILS)){
		diag("Couldn't set blkid partitionflags for %s (%s?)\n",dev,strerror(errno));
		goto err;
	}
	if(blkid_probe_enable_superblocks(bp,1)){
		diag("Couldn't enable blkid superprobe for %s (%s?)\n",dev,strerror(errno));
		goto err;
	}
	if(blkid_probe_set_superblocks_flags(bp,BLKID_SUBLKS_DEFAULT |
						BLKID_SUBLKS_VERSION)){
		diag("Couldn't set blkid superflags for %s (%s?)\n",dev,strerror(errno));
		goto err;
	}
	if(blkid_do_fullprobe(bp)){
		diag("Couldn't run blkid fullprobe for %s (%s?)\n",dev,strerror(errno));
		goto err;
	}
	n = blkid_probe_numof_values(bp);
	while(n--){
		blkid_probe_get_value(bp,n,&name,&val,&len);
		if(strcmp(name,"TYPE") == 0){
			if(strcmp(val,"swap") == 0){
				if((mnttype = strdup("swap")) == NULL){
					goto err;
				}
				if(d->swapprio == SWAP_INVALID){
					d->swapprio = SWAP_INACTIVE;
				}
			}else if(blkid_known_fstype(val)){
				if((mnttype = strdup(val)) == NULL){
					goto err;
				}
			}else{
				diag("Warning: unknown type %s for %s\n",val,dev);
			}
		}else if(strcmp(name,"SIZE") == 0){
			printf("SIZE: %s\n",val);
		}else if(strcmp(name,"UUID") == 0){
			if((uuid = strdup(val)) == NULL){
				goto err;
			}
		}else if(strcmp(name,"LABEL") == 0){
			if((label = strdup(val)) == NULL){
				goto err;
			}
		}else if(strcmp(name,"PART_ENTRY_UUID") == 0){
			if(d->layout == LAYOUT_PARTITION){
				if((partuuid = strdup(val)) == NULL){
					goto err;
				}
			}else{
				diag("PART_ENTRY_UUID on non-partition %s\n",d->name);
			}
		}else if(strcmp(name,"PART_ENTRY_NAME") == 0){
			if(d->layout == LAYOUT_PARTITION){
				mbstate_t ps;
				pname = malloc(sizeof(*pname) * (strlen(val) + 1));
				memset(&ps,0,sizeof(ps));
				mbsnrtowcs(pname,&val,strlen(val) + 1,strlen(val) + 1,&ps);
			}else{
				diag("PART_ENTRY_NAME on non-partition %s\n",d->name);
			}
		}else if(strcmp(name,"PART_ENTRY_TYPE") == 0){
			if(d->layout == LAYOUT_PARTITION){
				parttype = get_str_code(val);
			}else{
				diag("PART_ENTRY_TYPE on non-partition %s\n",d->name);
			}
		}else if(strcmp(name,"LOGICAL_SECTOR_SIZE") == 0){
			d->logsec = strtoull(val,NULL,0);
		}else if(strcmp(name,"PHYSICAL_SECTOR_SIZE") == 0){
			d->physsec = strtoull(val,NULL,0);
		}else{
			verbf("attr %s=%s for %s\n",name,val,dev);
		}
	}
	if(d->layout == LAYOUT_PARTITION){
		if(d->partdev.ptype == 0){
			d->partdev.ptype = parttype;
		}else if(!parttype || d->partdev.ptype != parttype){
			if(d->partdev.ptype){
				diag("Partition type changed (%04x->%04x)\n",
					d->partdev.ptype,parttype);
			}
			d->partdev.ptype = parttype;
		}
		if(d->partdev.uuid == NULL){
			d->partdev.uuid = partuuid;
		}else if(!partuuid || strcmp(d->partdev.uuid,partuuid)){
			if(d->partdev.uuid){
				diag("Partition UUID changed (%s->%s)\n",
					d->partdev.uuid,partuuid ? partuuid : "none");
			}
			free(d->partdev.uuid);
			d->partdev.uuid = partuuid;
		}
		if(d->partdev.pname == NULL){
			d->partdev.pname = pname;
		}else if(!pname || wcscmp(d->partdev.pname,pname)){
			if(d->partdev.pname){
				diag("Partition name changed (%ls->%ls)\n",
					d->partdev.pname,pname ? pname : L"none");
			}
			free(d->partdev.pname);
			d->partdev.pname = pname;
		}
	}
	if(d->mnttype == NULL){
		d->mnttype = mnttype;
	}else if(!mnttype || strcmp(mnttype,d->mnttype)){
		if(d->mnttype){
			diag("FS type changed (%s->%s)\n",
				d->mnttype,mnttype ? mnttype : "none");
		}
		free(d->mnttype);
		d->mnttype = mnttype;
	}else{
		free(mnttype);
	}
	if(d->uuid == NULL){
		d->uuid = uuid;
	}else if(!uuid || strcmp(uuid,d->uuid)){
		if(d->uuid){
			diag("FS UUID changed (%s->%s)\n",d->uuid,
					uuid ? uuid : "none");
		}
		free(d->uuid);
		d->uuid = uuid;
	}else{
		free(uuid);
	}
	if(d->label == NULL){
		d->label = label;
	}else if(!label || strcmp(label,d->label)){
		if(d->label){
			diag("FS label changed (%s->%s)\n",d->label,
					label ? label : "none");
		}
		free(d->label);
		d->label = label;
	}else{
		free(label);
	}
	if(sbp){
		*sbp = bp;
	}else{
		blkid_free_probe(bp);
	}
	return 0;

err:
	blkid_free_probe(bp);
	free(mnttype);
	free(label);
	free(uuid);
	return -1;
}

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysfs.h"
#include "mdadm.h"
#include "popen.h"
#include "growlight.h"

int explore_md_sysfs(device *d,int dirfd){
	unsigned degraded = 0;
	unsigned long rd;
	mdslave **enqm;
	char *syncpct;

	if((syncpct = get_sysfs_string(dirfd,"sync_completed")) == NULL){
		verbf("Warning: no 'sync_completed' content in mdadm device %s\n",d->name);
	}else if(strcmp(syncpct,"none") == 0){
		d->mddev.resync = 0;
	}else{
		// FIXME lex "%ju / %ju"
		d->mddev.resync = 1;
	}
	free(syncpct);
	// These files will be empty on incomplete arrays like the md0 that
	// sometimes pops up.
	if(get_sysfs_uint(dirfd,"raid_disks",&d->mddev.disks)){
		verbf("Warning: no 'raid_disks' content in mdadm device %s\n",d->name);
		d->mddev.disks = 0;
	}
	// Chunk size is only applicable for RAID[0456] and RAID10.
	// It is *not* set and *not* applicable for RAID1 or linear.
	if(get_sysfs_uint(dirfd,"chunk_size",&d->mddev.stride)){
		verbf("Warning: no 'chunk_size' content in mdadm device %s\n",d->name);
		d->mddev.stride = 0;
	}
	if(get_sysfs_uint(dirfd,"degraded",&d->mddev.degraded)){
		verbf("Warning: no 'degraded' content in mdadm device %s\n",d->name);
		d->mddev.degraded = 0;
	}
	if((d->mddev.level = get_sysfs_string(dirfd,"level")) == NULL){
		verbf("Warning: no 'level' content in mdadm device %s\n",d->name);
		d->mddev.level = 0;
	}
	if((d->revision = get_sysfs_string(dirfd,"metadata_version")) == NULL){
		verbf("Warning: no 'metadata_version' content in mdadm device %s\n",d->name);
	}
	// FIXME there's some archaic rules on mdadm devices making some of them
	// non-partitionable, but they're all partitionable after 2.6.38 or something
	if((d->mddev.pttable = strdup("mdp")) == NULL){
		return -1;
	}
	if((d->model = strdup("Linux mdadm")) == NULL){
		return -1;
	}
	enqm = &d->mddev.slaves;
	d->mddev.transport = AGGREGATE_UNKNOWN;
	for(rd = 0 ; rd < d->mddev.disks ; ++rd){
		char buf[NAME_MAX],lbuf[NAME_MAX],*c;
		device *subd;
		mdslave *m;
		int r;

		if(snprintf(buf,sizeof(buf),"rd%lu",rd) >= (int)sizeof(buf)){
			diag("Couldn't look up raid device %lu\n",rd);
			errno = ENAMETOOLONG;
			return -1;
		}
		r = readlinkat(dirfd,buf,lbuf,sizeof(lbuf));
		if((r < 0 && errno != ENOENT) || r >= (int)sizeof(lbuf)){
			int e = errno;

			diag("Couldn't look up slave %s (%s?)\n",buf,strerror(errno));
			errno = e;
			return -1;
		}else if(r < 0 && errno == ENOENT){ // missing/faulted device
			++degraded;
			continue;
		}
		lbuf[r] = '\0';
		if(strncmp(lbuf,"dev-",4)){
			diag("Couldn't get device from %s\n",lbuf);
			return -1;
		}
		if((c = strdup(lbuf + 4)) == NULL){
			return -1;
		}
		if((m = malloc(sizeof(*m))) == NULL){
			free(c);
			return -1;
		}
		m->name = c;
		m->next = NULL;
		*enqm = m;
		enqm = &m->next;
		lock_growlight();
		if((subd = lookup_device(c)) == NULL){
			unlock_growlight();
			return -1;
		}
		// m->component = subd;
		switch(subd->layout){
			case LAYOUT_NONE:
				if(d->mddev.transport == AGGREGATE_UNKNOWN){
					d->mddev.transport = subd->blkdev.transport;
				}else if(d->mddev.transport != subd->blkdev.transport){
					d->mddev.transport = AGGREGATE_MIXED;
				}
				break;
			case LAYOUT_MDADM:
				if(d->mddev.transport == AGGREGATE_UNKNOWN){
					d->mddev.transport = subd->mddev.transport;
				}else if(d->mddev.transport != subd->mddev.transport){
					d->mddev.transport = AGGREGATE_MIXED;
				}
				break;
			case LAYOUT_PARTITION:
				if(d->mddev.transport == AGGREGATE_UNKNOWN){
					d->mddev.transport = subd->partdev.parent->blkdev.transport;
				}else if(d->mddev.transport != subd->partdev.parent->blkdev.transport){
					d->mddev.transport = AGGREGATE_MIXED;
				}
				break;
			case LAYOUT_ZPOOL:
				if(d->mddev.transport == AGGREGATE_UNKNOWN){
					d->mddev.transport = subd->zpool.transport;
				}else if(d->mddev.transport != subd->zpool.transport){
					d->mddev.transport = AGGREGATE_MIXED;
				}
				break;
			default:
				diag("Unknown layout %d on %s\n",subd->layout,subd->name);
				break;
		}
		unlock_growlight();
	}
	d->mddev.degraded = degraded;
	if(d->mddev.resync && !d->mddev.degraded){
		d->mddev.degraded = 1;
	}
	// FIXME depends on aggregate type. number of non-parity disks
	if(d->mddev.stride){
		d->mddev.swidth = d->mddev.disks ? d->mddev.disks - 1 : 0;
	}else{
		d->mddev.swidth = 0;
	}
	return 0;
}

int destroy_mdadm(device *d){
	if(d == NULL){
		diag("Passed a NULL device\n");
		return -1;
	}
	if(d->layout != LAYOUT_MDADM){
		diag("%s is not an MD device\n",d->name);
		return -1;
	}
	if(vspopen_drain("mdadm --misc /dev/%s --stop",d->name)){
		return -1;
	}
	return 0;
}

static int
generic_mdadm_create(const char *name,const char *metadata,const char *level,
			char * const *comps,int num,int bitmap){
	char buf[BUFSIZ] = "";
	size_t pos;
	int z;

	pos = 0;
#define PREFIX "/dev/"
	for(z = 0 ; z < num ; ++z){
		if((unsigned)snprintf(buf + pos,sizeof(buf) - pos," %s%s",
				strcmp(*comps,"missing") ? "/dev/" : "",
				*comps) >= sizeof(buf) - pos){
			diag("Too many arguments for MD creation\n");
			return -1;
		}
		++comps;
		pos += strlen(buf + pos);
	}
#undef PREFIX
	// FIXME provide a way to let user control write intent bitmap
	return vspopen_drain("mdadm -C \"%s\" -e %s -l %s -N \"%s\" -n %d%s%s",
				name,metadata,level,name,num,
				bitmap ? " -b internal" : "",buf);
}

int make_mdraid0(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid0",comps,num,0);
}

int make_mdraid1(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid1",comps,num,1);
}

int make_mdraid4(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid4",comps,num,1);
}

int make_mdraid5(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid5",comps,num,1);
}

int make_mdraid6(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid6",comps,num,1);
}

int make_mdraid10(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid10",comps,num,1);
}

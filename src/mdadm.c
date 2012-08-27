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

	// These files will be empty on incomplete arrays like the md0 that
	// sometimes pops up.
	if(get_sysfs_uint(dirfd,"raid_disks",&d->mddev.disks)){
		verbf("Warning: no 'raid_disks' content in mdadm device %s\n",d->name);
		d->mddev.disks = 0;
	}
	if(get_sysfs_uint(dirfd,"degraded",&d->mddev.degraded)){
		verbf("Warning: no 'degraded' content in mdadm device %s\n",d->name);
	}
	if((d->mddev.level = get_sysfs_string(dirfd,"level")) == NULL){
		verbf("Warning: no 'level' content in mdadm device %s\n",d->name);
	}
	if((d->revision = get_sysfs_string(dirfd,"metadata_version")) == NULL){
		verbf("Warning: no 'level' content in mdadm device %s\n",d->name);
		d->mddev.level = 0;
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
		//device *subd;
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
		/*
		if((subd = lookup_device(c)) == NULL){
			free(c);
			return -1;
		}
		m->component = subd;
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
		*/
	}
	if(d->mddev.degraded != degraded){
		diag("%s had %lu degraded, %u missing\n",d->name,d->mddev.degraded,degraded);
		if(d->mddev.degraded < degraded){
			d->mddev.degraded = degraded;
		}
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
	if(vspopen_drain("mdadm --misc %s --stop",d->name)){
		return -1;
	}
	return 0;
}

static int
generic_mdadm_create(const char *name,const char *metadata,const char *level,
			char * const *comps,int num){
	char buf[BUFSIZ];
	size_t left,pos;
	int z;

	pos = 0;
	left = sizeof(buf) - 1;
	for(z = 0 ; z < num ; ++z){
		if(strlen(comps[z]) >= left){
			diag("Too many arguments for MD creation\n");
			return -1;
		}
		strcat(buf + pos,comps[z]);
		pos += strlen(comps[z]) + 1;
		buf[pos - 1] = ' ';
		left -= strlen(comps[z]) + 1;
	}
	buf[pos - 1] = '\0';
	// FIXME provide a way to let user control write intent bitmap
	return vspopen_drain("mdadm -c -e %s -l %s -N \"%s\" -n %d -b internal %s",
			metadata,level,name,num,buf);
}

int make_mdraid0(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid0",comps,num);
}

int make_mdraid1(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid1",comps,num);
}

int make_mdraid4(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid4",comps,num);
}

int make_mdraid5(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid5",comps,num);
}

int make_mdraid6(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid6",comps,num);
}

int make_mdraid10(const char *name,char * const *comps,int num){
	return generic_mdadm_create(name,"1.2","raid10",comps,num);
}

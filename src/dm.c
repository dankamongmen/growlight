#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "dm.h"
#include "sysfs.h"
#include "growlight.h"

int explore_dm_sysfs(device *d,int dirfd){
	unsigned long rd;
	mdslave **enqm;

	// These files will be empty on incomplete arrays like the md0 that
	// sometimes pops up.
	if(get_sysfs_uint(dirfd,"raid_disks",&d->mddev.disks)){
		verbf("Warning: no 'raid_disks' content in mdadm device %s\n",d->name);
		d->mddev.disks = 0;
	}
	if((d->mddev.level = get_sysfs_string(dirfd,"level")) == NULL){
		verbf("Warning: no 'level' content in mdadm device %s\n",d->name);
	}
	if((d->revision = get_sysfs_string(dirfd,"metadata_version")) == NULL){
		verbf("Warning: no 'level' content in mdadm device %s\n",d->name);
		d->mddev.level = 0;
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
			fprintf(stderr,"Couldn't look up raid device %lu\n",rd);
			errno = ENAMETOOLONG;
			return -1;
		}
		r = readlinkat(dirfd,buf,lbuf,sizeof(lbuf));
		if((r < 0 && errno != ENOENT) || r >= (int)sizeof(lbuf)){
			int e = errno;

			fprintf(stderr,"Couldn't look up slave %s (%s?)\n",buf,strerror(errno));
			errno = e;
			return -1;
		}else if(r < 0 && errno == ENOENT){
			// missing/faulted device -- represent somehow?
			continue;
		}
		lbuf[r] = '\0';
		if(strncmp(lbuf,"dev-",4)){
			fprintf(stderr,"Couldn't get device from %s\n",lbuf);
			return -1;
		}
		if((c = strdup(lbuf + 4)) == NULL){
			return -1;
		}
		if((subd = lookup_device(c)) == NULL){
			free(c);
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
				fprintf(stderr,"Unknown layout %d on %s\n",subd->layout,subd->name);
				break;
		}
	}
	return 0;
}

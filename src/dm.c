#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "dm.h"
#include "sysfs.h"
#include "growlight.h"

int explore_dm_sysfs(device *d,int dirfd){
	/*unsigned long rd;
	mdslave **enqm;*/

	d->dmdev.disks = 1;
	if((d->model = strdup("Linux devmapper")) == NULL){
		return -1;
	}
	if((d->dmdev.dmname = get_sysfs_string(dirfd,"name")) == NULL){
		verbf("Warning: no 'name' content in dm device %s\n",d->name);
	}
	d->dmdev.transport = AGGREGATE_UNKNOWN;
	// FIXME need to browse slaves/ subdirectory in sysfs entry
	/*enqm = &d->dmdev.slaves;
	for(rd = 0 ; rd < d->dmdev.disks ; ++rd){
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
		}else if(r < 0 && errno == ENOENT){
			// missing/faulted device -- represent somehow?
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
		//m->component = subd;
		lock_growlight();
		if((subd = lookup_device(c)) == NULL){
			unlock_growlight();
			return -1;
		}
		switch(subd->layout){
			case LAYOUT_NONE:
				if(d->dmdev.transport == AGGREGATE_UNKNOWN){
					d->dmdev.transport = subd->blkdev.transport;
				}else if(d->dmdev.transport != subd->blkdev.transport){
					d->dmdev.transport = AGGREGATE_MIXED;
				}
				break;
			case LAYOUT_MDADM:
				if(d->dmdev.transport == AGGREGATE_UNKNOWN){
					d->dmdev.transport = subd->dmdev.transport;
				}else if(d->dmdev.transport != subd->dmdev.transport){
					d->dmdev.transport = AGGREGATE_MIXED;
				}
				break;
			case LAYOUT_PARTITION:
				if(d->dmdev.transport == AGGREGATE_UNKNOWN){
					d->dmdev.transport = subd->partdev.parent->blkdev.transport;
				}else if(d->dmdev.transport != subd->partdev.parent->blkdev.transport){
					d->dmdev.transport = AGGREGATE_MIXED;
				}
				break;
			case LAYOUT_ZPOOL:
				if(d->dmdev.transport == AGGREGATE_UNKNOWN){
					d->dmdev.transport = subd->zpool.transport;
				}else if(d->dmdev.transport != subd->zpool.transport){
					d->dmdev.transport = AGGREGATE_MIXED;
				}
				break;
			default:
				diag("Unknown layout %d on %s\n",subd->layout,subd->name);
				break;
		}
		unlock_growlight();
	}*/
	return 0;
}

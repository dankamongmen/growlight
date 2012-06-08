#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <mdadm.h>
#include <string.h>
#include <unistd.h>

#include <sysfs.h>
#include <growlight.h>

int explore_md_sysfs(device *d,int dirfd){
	unsigned long rd;
	mdslave **enqm;

	// These files will be empty on incomplete arrays like the md0 that
	// sometimes pops up.
	if(get_sysfs_uint(dirfd,"raid_disks",&d->mddev.disks)){
		verbf("Warning: no 'raid_disks' content in mdadm device\n");
		d->mddev.disks = 0;
	}
	if((d->mddev.level = get_sysfs_string(dirfd,"level")) == NULL){
		verbf("Warning: no 'level' content in mdadm device\n");
		d->mddev.level = 0;
	}
	enqm = &d->mddev.slaves;
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
		if(r < 0 || r >= (int)sizeof(lbuf)){
			int e = errno;

			fprintf(stderr,"Couldn't look up slave %s (%s?)\n",buf,strerror(errno));
			errno = e;
			return -1;
		}
		lbuf[r] = '\0';
		if(strncmp(lbuf,"dev-",4)){
			fprintf(stderr,"Couldn't get device from %s\n",lbuf);
			return -1;
		}
		c = lbuf + 4;
		if((subd = lookup_device(c)) == NULL){
			return -1;
		}
		if((m = malloc(sizeof(*m))) == NULL){
			return -1;
		}
		m->next = NULL;
		*enqm = m;
		enqm = &m->next;
		m->component = subd;
	}
	return 0;
}

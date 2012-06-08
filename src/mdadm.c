#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <mdadm.h>
#include <unistd.h>

#include <sysfs.h>
#include <growlight.h>

int explore_md_sysfs(device *d,int dirfd){
	unsigned long rd;

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
	for(rd = 0 ; rd < d->mddev.disks ; ++rd){
		char buf[NAME_MAX];

		if(snprintf(buf,sizeof(buf),"rd%lu",rd) >= (int)sizeof(buf)){
			fprintf(stderr,"Couldn't look up raid device %lu\n",rd);
			errno = ENAMETOOLONG;
			return -1;
		}
		printf("***%s***\n",get_sysfs_string(dirfd,buf));
	}
	return 0;
}

#include <stdio.h>
#include <fcntl.h>
#include <mdadm.h>
#include <unistd.h>

#include <sysfs.h>
#include <growlight.h>

int explore_md_sysfs(device *d,int dirfd){
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
	return 0;
}

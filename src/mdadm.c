#include <stdio.h>
#include <fcntl.h>
#include <mdadm.h>
#include <unistd.h>

#include <sysfs.h>
#include <growlight.h>

int explore_md_sysfs(device *d,int dirfd){
	if(get_sysfs_uint(dirfd,"raid_disks",&d->mddev.disks)){
		return -1;
	}
	if((d->mddev.level = get_sysfs_string(dirfd,"level")) == NULL){
		return -1;
	}
	return 0;
}

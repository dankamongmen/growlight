#include <fcntl.h>
#include <mdadm.h>
#include <unistd.h>

int explore_md_sysfs(int dirfd){
	int fd;

	// FIXME use sysfs exploration functions from growlight.c
	if((fd = openat(dirfd,"raid_disks",O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return -1;
	}
	// FIXME read number of disks
	close(fd);
	return 0;
}

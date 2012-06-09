#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

#include <mmap.h>
#include <mounts.h>

int parse_mounts(const char *fn){
	off_t len;
	void *map;
	int fd;

	if((map = map_virt_file(fn,&fd,&len)) == MAP_FAILED){
		return -1;
	}
	close(fd);
	return 0;
}

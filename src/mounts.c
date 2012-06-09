#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <mounts.h>

int parse_mounts(const char *fn){
	int fd;

	if((fd = open(fn,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		int e = errno;
		fprintf(stderr,"Couldn't open %s (%s?)\n",fn,strerror(errno));
		errno = e;
		return -1;
	}
	close(fd);
	return 0;
}

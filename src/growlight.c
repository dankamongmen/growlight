#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <src/config.h>
#include <sys/inotify.h>

#define DISKS_BY_PATH "/dev/disk/by-path/"

static inline int
inotify_fd(void){
	int fd;

	if((fd = inotify_init1(IN_CLOEXEC)) < 0){
		fprintf(stderr,"Coudln't get inotify fd (%s?)\n",strerror(errno));
	}
	return fd;
}

static inline int
watch_dir(int fd,const char *dir){
	int wfd;

	wfd = inotify_add_watch(fd,dir,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
	if(wfd < 0){
		fprintf(stderr,"Coudln't get inotify watch (%s?)\n",strerror(errno));
		return -1;
	}else{
		printf("Watching %s on %d\n",dir,wfd);
	}
	return 0;
}

int main(void){
	int fd;

	printf("%s %s\n",PACKAGE,PACKAGE_VERSION);
	if((fd = inotify_fd()) < 0){
		return EXIT_FAILURE;
	}
	if(watch_dir(fd,DISKS_BY_PATH)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

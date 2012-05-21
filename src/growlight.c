#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <src/config.h>
#include <sys/inotify.h>

#define DISKS_BY_PATH "/dev/disk/by-path/"

// An (non-link) entry in the device hierarchy, representing a disk or
// partition.
typedef struct device {
	struct device *next;
	char name[PATH_MAX];
	char path[PATH_MAX];
	char id[PATH_MAX];
	char label[PATH_MAX];
	int major,minor;
} device;

static device *devs;

static inline device *
create_new_device(const char *name){
	device *d;

	if(strlen(name) >= sizeof(d->name)){
		return NULL;
	}
	if( (d = malloc(sizeof(*d))) ){
		memset(d,0,sizeof(*d));
		strcpy(d->name,name);
		// FIXME get major/minors
	}
	return d;
}

static device *
lookup_device(const char *name){
	device *d;

	for(d = devs ; d ; d = d->next){
		if(strcmp(name,d->name) == 0){
			return d;
		}
	}
	if( (d = create_new_device(name)) ){
		d->next = devs;
		devs = d;
	}
	return d;
}

static inline int
inotify_fd(void){
	int fd;

	if((fd = inotify_init1(IN_CLOEXEC)) < 0){
		fprintf(stderr,"Coudln't get inotify fd (%s?)\n",strerror(errno));
	}
	return fd;
}

static inline int
watch_dir(int fd,const char *dfp){
	struct dirent *d;
	int wfd,dfd,r;
	DIR *dir;

	wfd = inotify_add_watch(fd,dfp,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
	if(wfd < 0){
		fprintf(stderr,"Coudln't inotify on %s (%s?)\n",dfp,strerror(errno));
		return -1;
	}else{
		printf("Watching %s on %d\n",dfp,wfd);
	}
	if((dir = opendir(dfp)) < 0){
		fprintf(stderr,"Coudln't open %s (%s?)\n",dfp,strerror(errno));
		inotify_rm_watch(fd,wfd);
		return -1;
	}
	if((dfd = dirfd(dir)) < 0){
		fprintf(stderr,"Coudln't get fd on %s (%s?)\n",dfp,strerror(errno));
		inotify_rm_watch(fd,wfd);
		closedir(dir);
		return -1;
	}
	r = 0;
	while( errno = 0, (d = readdir(dir)) ){
		r = -1;
		if(d->d_type == DT_LNK){
			char buf[PATH_MAX];

			if(readlinkat(dfd,d->d_name,buf,sizeof(buf)) < 0){
				fprintf(stderr,"Couldn't read link at %s/%s (%s?)\n",
					dfp,d->d_name,strerror(errno));
			}else{
				const device *dev;

				if((dev = lookup_device(d->d_name)) == NULL){
					fprintf(stderr,"Couldn't look up %s (%s?)\n",
							d->d_name,strerror(errno));
					break;
				}
				printf("%s -> %s\n",d->d_name,buf);
			}
		}
		r = 0;
	}
	if(errno){
		fprintf(stderr,"Error reading %s (%s?)\n",dfp,strerror(errno));
		r = -1;
	}
	closedir(dir);
	return r;
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

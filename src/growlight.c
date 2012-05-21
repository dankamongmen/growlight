#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <src/config.h>
#include <sys/inotify.h>

#define DISKS_PREFIX "/dev/disk"
#define DISKS_BY_ID "/by-id/"
#define DISKS_BY_PATH "/by-path/"

static unsigned verbose;

static inline int
verbf(const char *fmt,...){
	va_list ap;
	int v;

	va_start(ap,fmt);
	if(verbose){
		v = vprintf(fmt,ap);
	}else{
		v = 0;
	}
	va_end(ap);
	return v;
}

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

static void
free_devtable(void){
	device *d;

	while( (d = devs) ){
		devs = d->next;
		free(d);
	}
}

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
		verbf("Watching %s on %d\n",dfp,wfd);
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
			char buf[PATH_MAX] = "";

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
				verbf("%s -> %s\n",d->d_name,buf);
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

static void
usage(const char *name,int status){
	FILE *fp = status == EXIT_SUCCESS ? stdout : stderr;

	fprintf(fp,"usage: %s [ -h|--help ] [ -v|--verbose ]\n",name);
	exit(status);
}

int main(int argc,char **argv){
	static const struct option ops[] = {
		{
			.name = "help",
			.has_arg = 0,
			.flag = NULL,
			.val = 'h',
		},{
			.name = "verbose",
			.has_arg = 0,
			.flag = NULL,
			.val = 'v',
		},{
			.name = NULL,
			.has_arg = 0,
			.flag = NULL,
			.val = 0,
		},
	};
	int fd,opt,longidx;

	opterr = 1;
	while((opt = getopt_long(argc,argv,"hv",ops,&longidx)) >= 0){
		switch(opt){
		case 'h':{
			usage(argv[0],EXIT_SUCCESS);
			break;
		}case 'v':{
			verbose = 1;
			break;
		}case ':':{
			fprintf(stderr,"Option requires argument: '%c'\n",optopt);
			usage(argv[0],EXIT_FAILURE);
			break;
		}case '?':{
			fprintf(stderr,"Unknown option: '%c'\n",optopt);
			usage(argv[0],EXIT_FAILURE);
			break;
		}default:{
			fprintf(stderr,"Unknown option: '%c'\n",optopt);
			usage(argv[0],EXIT_FAILURE);
			break;
		} }
	}
	printf("%s %s\n",PACKAGE,PACKAGE_VERSION);
	if((fd = inotify_fd()) < 0){
		return EXIT_FAILURE;
	}
	if(watch_dir(fd,DISKS_PREFIX DISKS_BY_PATH)){
		free_devtable();
		return EXIT_FAILURE;
	}
	if(watch_dir(fd,DISKS_PREFIX DISKS_BY_ID)){
		free_devtable();
		return EXIT_FAILURE;
	}
	free_devtable();
	return EXIT_SUCCESS;
}

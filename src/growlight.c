#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <blkid.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <scsi/sg.h>
#include <sys/stat.h>
#include <scsi/scsi.h>
#include <sys/ioctl.h>
#include <src/config.h>
#include <sys/inotify.h>

#include <libblkid.h>
#include <growlight.h>

#define DEVROOT "/dev/"
#define SYSROOT "/sys/block/"

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
	char name[PATH_MAX];	// Entry in /dev or /sys/block
	char path[PATH_MAX];	// Device topology, not filesystem
	char *model,*revision;  // Arbitrary strings
	unsigned logsec;	// Logical sector size
	unsigned physsec;	// Physical sector size
} device;

static device *devs;
static int devfd = -1; // Hold a reference to DEVROOT
static int sysfd = -1; // Hold a reference to SYSROOT

static void
free_devtable(void){
	device *d;

	while( (d = devs) ){
		devs = d->next;
		free(d);
	}
	close(devfd);
	close(sysfd);
	devfd = sysfd = -1;
}

	/*if(fstatat(fd,"md",&sbuf,AT_NO_AUTOMOUNT) == 0){
		if(S_ISDIR(sbuf.st_mode)){
			raid = 1;
		}
	}*/

	/* if(realdev){
				struct sg_io_hdr sg;
#define INQ_REPLY_LEN 96
				unsigned char cmd[6] = {
					INQUIRY,0,0,0,0x24,0
				};
				unsigned char sense[INQ_REPLY_LEN];
#undef INQ_REPLY_LEN
				unsigned char buf[512];
				int r;
				memset(&sg,0,sizeof(sg));
				sg.interface_id = 'S'; // SCSI
				sg.dxfer_direction = SG_DXFER_FROM_DEV;
				sg.cmd_len = sizeof(cmd);
				//sg.mx_sb_len = sizeof(sense);
				sg.mx_sb_len = 32;
				sg.iovec_count = 0;
				//sg.dxfer_len = sizeof(sense);
				sg.dxfer_len = 32;
				sg.cmdp = cmd;
				sg.sbp = sense;
				sg.usr_ptr = buf;
#define SCSI_TIMEOUT_MS 20000
				sg.timeout = SCSI_TIMEOUT_MS;
#undef SCSI_TIMEOUT_MS
				//sg.flags = SG_FLAG_DIRECT_IO;
				r = ioctl(fd,SG_IO,&sg,sizeof(sg),buf,sizeof(buf));
				printf("IOCTL: %d\n",r);
				close(fd);
				if(r != 0){
					fprintf(stderr,"Couldn't run SG_IO on %s (%s?)\n",name,strerror(errno));
					return NULL;
				}
			} */

// FIXME use libudev for this crap
// FIXME sysfs is UTF-8 not ASCII
char *get_sysfs_string(int dirfd,const char *node){
	char buf[512]; // FIXME
	ssize_t r;
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return NULL;
	}
	if((r = read(fd,buf,sizeof(buf))) <= 0){
		int e = errno;
		close(fd);
		errno = e;
		return NULL;
	}
	if((size_t)r >= sizeof(buf) || buf[r - 1] != '\n'){
		close(fd);
		errno = ENAMETOOLONG;
		return NULL;
	}
	close(fd);
	buf[r - 1] = '\0';
	return strdup(buf);
}

int get_sysfs_bool(int dirfd,const char *node,unsigned *b){
	char buf[512]; // FIXME
	ssize_t r;
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return -1;
	}
	if((r = read(fd,buf,sizeof(buf))) <= 0){
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if((size_t)r >= sizeof(buf) || buf[r - 1] != '\n'){
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	close(fd);
	buf[r - 1] = '\0';
	*b = strcmp(buf,"0") ? 1 : 0;
	return 0;
}

static inline device *
create_new_device(const char *name){
	unsigned realdev = 0,physsec = 0,logsec = 0,mddev = 0,removable = 0;
	char *model = NULL,*rev = NULL;
	char devbuf[PATH_MAX] = "";
	char buf[PATH_MAX] = "";
	struct stat sbuf;
	//DIR *sdevdir;
	device *d;
	int fd,sdevfd;

	if(strlen(name) >= sizeof(d->name)){
		fprintf(stderr,"Name too long: %s\n",name);
		return NULL;
	}
	if(readlinkat(sysfd,name,buf,sizeof(buf)) < 0){
		fprintf(stderr,"Couldn't read link at %s/%s (%s?)\n",
			SYSROOT,name,strerror(errno));
	}else{
		verbf("%s -> %s\n",name,buf);
	}
	if((fd = openat(sysfd,buf,O_RDONLY|O_CLOEXEC)) < 0){
		fprintf(stderr,"Couldn't open link at %s/%s (%s?)\n",
			SYSROOT,buf,strerror(errno));
		return NULL;
	}
	if(get_sysfs_bool(fd,"removable",&removable)){
		fprintf(stderr,"Couldn't determine removability for %s (%s?)\n",name,strerror(errno));
	}
	// Check for "device" to determine if it's real or virtual
	if((sdevfd = openat(fd,"device",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
		realdev = 1;
		if((model = get_sysfs_string(sdevfd,"model")) == NULL){
			fprintf(stderr,"Couldn't get a model for %s (%s?)\n",name,strerror(errno));
		}
		if((rev = get_sysfs_string(sdevfd,"rev")) == NULL){
			fprintf(stderr,"Couldn't get a revision for %s (%s?)\n",name,strerror(errno));
		}
		verbf("\tModel: %s revision %s\n",model,rev);
		close(sdevfd);
	}
	// Check for "md" to determine if it's an MDADM device
	if(fstatat(fd,"md",&sbuf,AT_NO_AUTOMOUNT) == 0){
		mddev = 1;
	}
	if(close(fd)){
		fprintf(stderr,"Couldn't close fd %d (%s?)\n",fd,strerror(errno));
		free(model); free(rev);
		return NULL;
	}
	if((unsigned)snprintf(devbuf,sizeof(devbuf),DEVROOT"%s",name) >= sizeof(devbuf)){
		fprintf(stderr,"Couldn't construct dev path for "DEVROOT"%s\n",name);
		free(model); free(rev);
		return NULL;
	}
	if(realdev || mddev){
		blkid_parttable ptbl;
		blkid_topology tpr;
		blkid_partlist ppl;
		blkid_probe pr;
		int pars;

		// FIXME move all this to its own function
		if(probe_blkid_dev(devbuf,&pr) == 0){
			if( (ppl = blkid_probe_get_partitions(pr)) ){
				if((ptbl = blkid_partlist_get_table(ppl)) == NULL){
					fprintf(stderr,"Couldn't probe partition table of %s (%s?)\n",name,strerror(errno));
					close(fd);
					free(model); free(rev);
					blkid_free_probe(pr);
					return NULL;
				}
				pars = blkid_partlist_numof_partitions(ppl);
				verbf("\t%d partition%s, table type %s\n",
						pars,pars == 1 ? "" : "s",
						blkid_parttable_get_type(ptbl));
			}else{
				verbf("\tNo partition table\n");
			}
			if((tpr = blkid_probe_get_topology(pr)) == NULL){
				fprintf(stderr,"Couldn't probe topology of %s (%s?)\n",name,strerror(errno));
				close(fd);
				free(model); free(rev);
				blkid_free_probe(pr);
				return NULL;
			}
			// FIXME errorchecking!
			logsec = blkid_topology_get_logical_sector_size(tpr);
			physsec = blkid_topology_get_physical_sector_size(tpr);
			verbf("\tLogical sector size: %uB Physical sector size: %uB\n",logsec,physsec);
			blkid_free_probe(pr);
		}else if(!removable || errno != ENOMEDIUM){
			fprintf(stderr,"Couldn't probe %s (%s?)\n",name,strerror(errno));
			close(fd);
			free(model); free(rev);
			return NULL;
		}else{
			verbf("\tDevice is unloaded\n");
		}
	}
	if( (d = malloc(sizeof(*d))) ){
		memset(d,0,sizeof(*d));
		strcpy(d->name,name);
		d->logsec = logsec;
		d->physsec = physsec;
		d->revision = rev;
		d->model = model;
	}else{
		fprintf(stderr,"Couldn't look up %s (%s?)\n",name,strerror(errno));
		free(model); free(rev);
	}
	return d;
}

// Strips leading "/dev/"s, "../"s and "./"s, for better or worse. What's left
// must be an entry in /sys/block (and should probably be one in /dev, but
// we can index back with major/minor numbers...I think).
device *lookup_device(const char *name){
	device *d;
	size_t s;

	do{
		if(strncmp(name,"/",1) == 0){
			s = 1;
		}else if(strncmp(name,"./",2) == 0){
			s = 2;
		}else if(strncmp(name,"../",3) == 0){
			s = 3;
		}else if(strncmp(name,"dev/",4) == 0){
			s = 4;
		}else{
			s = 0;
		}
		name += s;
	}while(s);
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
	DIR *dir;
	int wfd,r;
	int dfd;

	wfd = inotify_add_watch(fd,dfp,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
	if(wfd < 0){
		fprintf(stderr,"Coudln't inotify on %s (%s?)\n",dfp,strerror(errno));
		return -1;
	}else{
		verbf("Watching %s on fd %d\n",dfp,wfd);
	}
	r = 0;
	if((dir = opendir(dfp)) == NULL){
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
	while( errno = 0, (d = readdir(dir)) ){
		r = -1;
		if(d->d_type == DT_LNK){
			const device *dev;

			if((dev = lookup_device(d->d_name)) == NULL){
				break;
			}
		}
		r = 0;
	}
	if(r == 0 && errno){
		fprintf(stderr,"Error reading %s (%s?)\n",dfp,strerror(errno));
		r = -1;
	}
	closedir(dir);
	return r;
}

static void
version(const char *name,int status){
	FILE *fp = status == EXIT_SUCCESS ? stdout : stderr;

	fprintf(fp,"%s version %s\n",name,VERSION);
	exit(status);
}

static void
usage(const char *name,int status){
	FILE *fp = status == EXIT_SUCCESS ? stdout : stderr;

	fprintf(fp,"usage: %s [ -h|--help ] [ -v|--verbose ] [ -V|--version ]\n",name);
	exit(status);
}

static int
get_dir_fd(DIR **dir,const char *root){
	int fd;

	if((*dir = opendir(root)) == NULL){
		fprintf(stderr,"Couldn't open directory at %s (%s?)\n",root,strerror(errno));
		return -1;
	}
	if((fd = dirfd(*dir)) < 0){
		fprintf(stderr,"Couldn't get dirfd at %s (%s?)\n",root,strerror(errno));
		closedir(*dir);
		*dir = NULL;
	}
	return fd;
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
			.name = "version",
			.has_arg = 0,
			.flag = NULL,
			.val = 'V',
		},{
			.name = NULL,
			.has_arg = 0,
			.flag = NULL,
			.val = 0,
		},
	};
	int fd,opt,longidx;
	DIR *sdir;

	opterr = 0; // disallow getopt(3) diagnostics to stderr
	while((opt = getopt_long(argc,argv,"hvV",ops,&longidx)) >= 0){
		switch(opt){
		case 'h':{
			usage(argv[0],EXIT_SUCCESS);
			break;
		}case 'v':{
			verbose = 1;
			break;
		}case 'V':{
			version(argv[0],EXIT_SUCCESS);
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
	printf("%s %s (libblkid %s)\n",PACKAGE,PACKAGE_VERSION,BLKID_VERSION);
	if(chdir(SYSROOT)){
		fprintf(stderr,"Couldn't cd to %s (%s?)\n",SYSROOT,strerror(errno));
		return EXIT_FAILURE;
	}
	if((sysfd = get_dir_fd(&sdir,SYSROOT)) < 0){
		return EXIT_FAILURE;
	}
	if((devfd = get_dir_fd(&sdir,DEVROOT)) < 0){
		return EXIT_FAILURE;
	}
	/*if(load_blkid_superblocks()){
		fprintf(stderr,"Error initializing libblkid (%s?)\n",strerror(errno));
		free_devtable();
		return EXIT_FAILURE;
	}*/
	if((fd = inotify_fd()) < 0){
		return EXIT_FAILURE;
	}
	if(watch_dir(fd,SYSROOT)){
		free_devtable();
		return EXIT_FAILURE;
	}
	close_blkid();
	free_devtable();
	return EXIT_SUCCESS;
}

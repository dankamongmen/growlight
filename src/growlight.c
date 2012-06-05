#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <blkid.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <scsi/sg.h>
#include <langinfo.h>
#include <sys/stat.h>
#include <scsi/scsi.h>
#include <sys/ioctl.h>
#include <src/config.h>
#include <sys/inotify.h>

#include <pci/pci.h>
#include <pci/header.h>

#include <libblkid.h>
#include <growlight.h>

#define SYSROOT "/sys/block/"

static unsigned verbose;
static struct pci_access *pciacc;

static int verbf(const char *,...) __attribute__ ((format (printf,1,2)));

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

// A block device controller.
typedef struct controller {
	// FIXME if libpci doesn't know about the device, we still ought use
	// a name determined via inspection of sysfs, just as we do for disks
	char *name;		// From libpci database
	enum {
		BUS_VIRTUAL,
		BUS_PCIe,
	} bus;
	// Union parameterized on bus type
	union {
		struct {
			// PCIe theoretical speeds reach (1 transfer == 1 bit):
			//
			//  1.0: 2.5GT/s each way
			//  2.0: 5GT/s each way
			//  3.0: 8GT/s each way
			//
			// 1.0 and 2.0 use 8/10 encoding, while 3.0 uses 8/128
			// encoding. 1.0 thus gives you a peak of 250MB/s/lane,
			// and 2.0 offers 500MB/s/lane. Further overheads can
			// reduce the useful throughput.
			unsigned gen;
			// A physical slot can be incompletely wired, allowing
			// a card of n lanes to be used in a slot with only m
			// electronically-wired lanes, n > m.
			//
			//  lanes_cap: card capabilities
			//  lanes_sta: negotiated number of PCIe lanes
			unsigned lanes_cap,lanes_neg;
			// PCIe topological addressing
			unsigned domain,bus,dev,func;
		} pcie;
	};
	struct controller *next;
} controller;

static controller *controllers;

#define PCI_EXP_LNKSTA		0x12
#define  PCI_EXP_LNKSTA_SPEED   0x000f  /* Negotiated Link Speed */
#define  PCI_EXP_LNKSTA_WIDTH   0x03f0  /* Negotiated Link Width */
#define  PCI_EXP_LNKSTA_TR_ERR  0x0400  /* Training Error (obsolete) */
#define  PCI_EXP_LNKSTA_TRAIN   0x0800  /* Link Training */
#define  PCI_EXP_LNKSTA_SL_CLK  0x1000  /* Slot Clock Configuration */
#define  PCI_EXP_LNKSTA_DL_ACT  0x2000  /* Data Link Layer in DL_Active State */
#define  PCI_EXP_LNKSTA_BWMGMT  0x4000  /* Bandwidth Mgmt Status */
#define  PCI_EXP_LNKSTA_AUTBW   0x8000  /* Autonomous Bandwidth Mgmt Status */
#define FLAG(x,y) ((x & y) ? '+' : '-')


static inline const char *
link_speed(int speed){
	switch(speed){
		case 1: return "2.5GT/s";
		case 2: return "5GT/s";
		case 3: return "8GT/s";
		default: return "unknown";
	}
}

static const controller *
find_pcie_controller(unsigned domain,unsigned bus,unsigned dev,unsigned func){
	const controller *cur;

	for(cur = controllers ; cur ; cur = cur->next){
		if(cur->bus != BUS_PCIe){
			continue;
		}
		if(cur->pcie.domain != domain || cur->pcie.bus != bus){
			continue;
		}
		if(cur->pcie.dev != dev || cur->pcie.func != func){
			continue;
		}
		break;
	}
	if(cur == NULL){
		struct pci_dev *pcidev;
		char buf[BUFSIZ],*rbuf;
		controller *c;

		if((pcidev = pci_get_dev(pciacc,domain,bus,dev,func)) == NULL){
			fprintf(stderr,"Couldn't look up PCI device\n");
			return NULL;
		}
		assert(pci_fill_info(pcidev,PCI_FILL_IDENT|PCI_FILL_IRQ|PCI_FILL_BASES|PCI_FILL_ROM_BASE|
						PCI_FILL_CAPS|PCI_FILL_EXT_CAPS|
						PCI_FILL_SIZES|PCI_FILL_RESCAN));
		if( (rbuf = pci_lookup_name(pciacc,buf,sizeof(buf),PCI_LOOKUP_VENDOR|PCI_LOOKUP_DEVICE,
						pcidev->vendor_id,pcidev->device_id)) ){
		}
		if( (c = malloc(sizeof(*c))) ){
			struct pci_cap *pcicap;
			uint32_t data;

			c->name = NULL;
			c->bus = BUS_PCIe;
			c->pcie.domain = domain;
			c->pcie.bus = bus;
			c->pcie.dev = dev;
			c->pcie.func = func;
			//verbf("\tPCI domain: %lu bus: %lu dev: %lu func: %lu\n",domain,bus,dev,func);
			/* Get the relevant address pointer */
			data = 0;
			if( (pcicap = pci_find_cap(pcidev,PCI_CAP_ID_EXP,PCI_CAP_NORMAL)) ){
				data = pci_read_word(pcidev,pcicap->addr + PCI_EXP_LNKSTA);
			}else if( (pcicap = pci_find_cap(pcidev,PCI_CAP_ID_MSI,PCI_CAP_NORMAL)) ){
				// FIXME?
			}
			if(data){
				printf("\tLnkSta:\tSpeed %s, Width x%d\n",
					link_speed(data & PCI_EXP_LNKSTA_SPEED),
					(data & PCI_EXP_LNKSTA_WIDTH) >> 4u);
			}
			if((c->name = strdup(rbuf)) == NULL){
				// FIXME?
			}
		}
		pci_free_dev(pcidev);
		cur = c;
	}
	return cur;
}

// An (non-link) entry in the device hierarchy, representing a block device.
typedef struct device {
	struct device *next;
	char name[PATH_MAX];		// Entry in /dev or /sys/block
	char path[PATH_MAX];		// Device topology, not filesystem
	char *model,*revision;		// Arbitrary UTF-8 strings
	const controller *controller;	// Primary controller route
					// FIXME how to deal with multipath?
	unsigned logsec;		// Logical sector size
	unsigned physsec;		// Physical sector size
	struct {
		unsigned realdev: 1;	// Is itself a real block device 
		unsigned removable: 1;	// Removable media
	};
	enum {
		LAYOUT_NONE,
		LAYOUT_MDADM,
	} layout;
} device;

static device *devs;
static int sysfd = -1; // Hold a reference to SYSROOT

static void
free_device(device *d){
	if(d){
		free(d->model);
		free(d->revision);
	}
}

static void
free_devtable(void){
	device *d;

	while( (d = devs) ){
		devs = d->next;
		free_device(d);
		free(d);
	}
	close(sysfd);
	sysfd = -1;
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
static char *
get_sysfs_string(int dirfd,const char *node){
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

static unsigned
sysfs_exist_p(int dirfd,const char *node){
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return 0;
	}
	close(fd);
	return 1;
}

static int
get_sysfs_bool(int dirfd,const char *node,unsigned *b){
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

// Pass a directory handle fd, and the bare name of the device
int explore_sysfs_node(int fd,const char *name,device *d){
	struct dirent *dire;
	unsigned b;
	int sdevfd;
	DIR *dir;

	// do *not* call closedir(3) on dir: doing so will close(2) fd.
	if((dir = fdopendir(fd)) == NULL){
		fprintf(stderr,"Couldn't get DIR * from fd %d for %s (%s?)\n",
				fd,name,strerror(errno));
		return -1;
	}
	while(errno = 0, (dire = readdir(dir)) ){
		int subfd;

		if(dire->d_type == DT_DIR){
			// Check for "md" to determine if it's an MDADM device
			if(strcmp(dire->d_name,"md") == 0){
				d->layout = LAYOUT_MDADM;
			}else if((subfd = openat(fd,dire->d_name,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
				if(sysfs_exist_p(subfd,"partition")){
					verbf("\tPartition at %s\n",dire->d_name);
				}
				close(subfd);
			}else{
				fprintf(stderr,"Couldn't open directory at %s for %s (%s?)\n",
						dire->d_name,name,strerror(errno));
				return -1;
			}
		}
	}
	if(errno){
		fprintf(stderr,"Error walking sysfs:%s (%s?)\n",name,strerror(errno));
		return -1;
	}
	if(get_sysfs_bool(fd,"removable",&b)){
		fprintf(stderr,"Couldn't determine removability for %s (%s?)\n",name,strerror(errno));
	}else{
		d->removable = !!b;
	}
	// Check for "device" to determine if it's real or virtual
	if((sdevfd = openat(fd,"device",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
		d->realdev = 1;
		if((d->model = get_sysfs_string(sdevfd,"model")) == NULL){
			fprintf(stderr,"Couldn't get a model for %s (%s?)\n",name,strerror(errno));
		}
		if((d->revision = get_sysfs_string(sdevfd,"rev")) == NULL){
			fprintf(stderr,"Couldn't get a revision for %s (%s?)\n",name,strerror(errno));
		}
		verbf("\tModel: %s revision %s\n",d->model,d->revision);
		close(sdevfd);
	}
	return 0;
}

static int
parse_pci_busid(const char *busid,unsigned long *domain,unsigned long *bus,
                                unsigned long *dev,unsigned long *func){
        const char *cur;
        char *e;

        // FIXME clean this cut-and-paste crap up
        cur = busid;
        if(*cur == '-'){ // strtoul() admits leading negations
                return -1;
        }
        if(strtoul(cur,&e,16) == ULONG_MAX){
                return -1;
        }
        if(*e != ':'){
                return -1;
        }
        cur = e + 1;
        if(*cur == '-'){ // strtoul() admits leading negations
                return -1;
        }
        if(strtoul(cur,&e,16) == ULONG_MAX){
                return -1;
        }
        if(*e != '/'){
                return -1;
        }
	*domain = *bus = *dev = *func = 0; // FIXME purge
	// FIXME hack! we ought check to see if the PCI device we just
	// resolved is a bridge, and if so, keep going. instead, check
	// whatever comes next. no bueno!
	while(cur = e + 1, !isalpha(*cur)){
		if(*cur == '-'){ // strtoul() admits leading negations
			return -1;
		}
		if((*domain = strtoul(cur,&e,16)) == ULONG_MAX){
			return -1;
		}
		if(*e != ':'){
			return -1;
		}
		cur = e + 1;
		if(*cur == '-'){ // strtoul() admits leading negations
			return -1;
		}
		if((*bus = strtoul(cur,&e,16)) == ULONG_MAX){
			return -1;
		}
		if(*e != ':'){
			return -1;
		}
		cur = e + 1;
		if(*cur == '-'){ // strtoul() admits leading negations
			return -1;
		}
		if((*dev = strtoul(cur,&e,16)) == ULONG_MAX){
			return -1;
		}
		if(*e != '.'){
			return -1;
		}
		cur = e + 1;
		if(*cur == '-'){ // strtoul() admits leading negations
			return -1;
		}
		if((*func = strtoul(cur,&e,16)) == ULONG_MAX){
			return -1;
		}
		if(*e != '/'){
			return -1;
		}
	}
        return 0;
}

// Takes the sysfs link as read when dereferencing /sys/block/*. Only works
// for virtual/PCI currently.
static int
parse_bus_topology(const char *fn,device *d){
	unsigned long domain,bus,dev,func;
	const char *pci;

	if(strstr(fn,"/devices/virtual/")){
		//d->controller = strdup("Virtual device"); FIXME
		return 0;
	}
	if((pci = strstr(fn,"/devices/pci")) == NULL){
		// FIXME d->controller = strdup("Unknown bus type");
		return 0;
	}
	pci += strlen("/devices/pci");
	if(parse_pci_busid(pci,&domain,&bus,&dev,&func)){
		fprintf(stderr,"Couldn't extract PCI address from %s\n",pci);
		return -1;
	}
	if((d->controller = find_pcie_controller(domain,bus,dev,func)) == NULL){
		return -1;
	}
	return 0;
}


static inline device *
create_new_device(const char *name){
	char buf[PATH_MAX] = "";
	device *d,dd;
	int fd;

	memset(&dd,0,sizeof(dd));
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
	if(parse_bus_topology(buf,&dd)){
		fprintf(stderr,"Couldn't get physical bus topology for %s\n",name);
		return NULL;
	}else if(dd.controller){
		verbf("\tController: %s\n",dd.controller->name);
	}
	if((fd = openat(sysfd,buf,O_RDONLY|O_CLOEXEC)) < 0){
		fprintf(stderr,"Couldn't open link at %s/%s (%s?)\n",
			SYSROOT,buf,strerror(errno));
		free_device(&dd);
		return NULL;
	}
	if(explore_sysfs_node(fd,name,&dd)){
		close(fd);
		free_device(&dd);
		return NULL;
	}
	if(close(fd)){
		fprintf(stderr,"Couldn't close fd %d (%s?)\n",fd,strerror(errno));
		free_device(&dd);
		return NULL;
	}
	if(dd.realdev || (dd.layout == LAYOUT_MDADM)){
		char devbuf[PATH_MAX];
		blkid_parttable ptbl;
		blkid_topology tpr;
		blkid_partlist ppl;
		blkid_probe pr;
		int pars;

		snprintf(devbuf,sizeof(devbuf),"/dev/%s",name);
		// FIXME move all this to its own function
		if(probe_blkid_dev(devbuf,&pr) == 0){
			if( (ppl = blkid_probe_get_partitions(pr)) ){
				if((ptbl = blkid_partlist_get_table(ppl)) == NULL){
					fprintf(stderr,"Couldn't probe partition table of %s (%s?)\n",name,strerror(errno));
					close(fd);
					free_device(&dd);
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
				free_device(&dd);
				blkid_free_probe(pr);
				return NULL;
			}
			// FIXME errorchecking!
			dd.logsec = blkid_topology_get_logical_sector_size(tpr);
			dd.physsec = blkid_topology_get_physical_sector_size(tpr);
			verbf("\tLogical sector size: %uB Physical sector size: %uB\n",
					dd.logsec,dd.physsec);
			blkid_free_probe(pr);
		}else if(!dd.removable || errno != ENOMEDIUM){
			fprintf(stderr,"Couldn't probe %s (%s?)\n",name,strerror(errno));
			close(fd);
			free_device(&dd);
			return NULL;
		}else{
			verbf("\tDevice is unloaded/inaccessible\n");
		}
	}
	if( (d = malloc(sizeof(*d))) ){
		*d = dd;
		strcpy(d->name,name);
	}else{
		fprintf(stderr,"Couldn't look up %s (%s?)\n",name,strerror(errno));
		free_device(&dd);
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

static int
pci_system_init(void){
	if((pciacc = pci_alloc()) == NULL){
		return -1;
	}
	pci_init(pciacc);
	pci_scan_bus(pciacc);
	return 0;
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
	const char *enc;
	DIR *sdir;

	if(setlocale(LC_ALL,"") == NULL){
		fprintf(stderr,"Couldn't set locale (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if((!(enc = nl_langinfo(CODESET))) || strcmp(enc,"UTF-8")){
		fprintf(stderr,"Output isn't UTF-8, aborting\n");
		return EXIT_FAILURE;
	}
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
	printf("%s %s (libblkid %s, libpci 0x%x)\n",PACKAGE,PACKAGE_VERSION,
			BLKID_VERSION,PCI_LIB_VERSION);
	if(pci_system_init()){
		fprintf(stderr,"Couldn't init libpci (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if(chdir(SYSROOT)){
		fprintf(stderr,"Couldn't cd to %s (%s?)\n",SYSROOT,strerror(errno));
		return EXIT_FAILURE;
	}
	if((sysfd = get_dir_fd(&sdir,SYSROOT)) < 0){
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
	pci_cleanup(pciacc);
	return EXIT_SUCCESS;
}

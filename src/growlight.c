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

#include <sg.h>
#include <swap.h>
#include <mdadm.h>
#include <sysfs.h>
#include <mounts.h>
#include <libblkid.h>
#include <growlight.h>

#define SYSROOT "/sys/block/"
#define MOUNTS	"/proc/mounts"
#define DEVROOT "/dev"
#define DEVBYID DEVROOT "/disk/by-id/"

static unsigned verbose;
static struct pci_access *pciacc;
static int sysfd = -1; // Hold a reference to SYSROOT
static int devfd = -1; // Hold a reference to DEVROOT

static controller unknown_bus = {
	.name = "Unknown controller",
	.bus = BUS_UNKNOWN,
};

static controller virtual_bus = {
	.name = "Virtual device",
	.next = &unknown_bus,
	.bus = BUS_VIRTUAL,
};

static controller *controllers = &virtual_bus;

int verbf(const char *fmt,...){
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

static controller *
find_pcie_controller(unsigned domain,unsigned bus,unsigned dev,unsigned func){
	controller *cur;

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
			fprintf(stderr,"Couldn't look up PCIe device\n");
			return NULL;
		}
		assert(pci_fill_info(pcidev,PCI_FILL_IDENT|PCI_FILL_IRQ|PCI_FILL_BASES|PCI_FILL_ROM_BASE|
						PCI_FILL_CAPS|PCI_FILL_EXT_CAPS|
						PCI_FILL_SIZES|PCI_FILL_RESCAN));
		rbuf = pci_lookup_name(pciacc,buf,sizeof(buf),PCI_LOOKUP_VENDOR|PCI_LOOKUP_DEVICE,
						pcidev->vendor_id,pcidev->device_id);
		if(rbuf == NULL){
			rbuf = "Unknown PCIe device\n"; // FIXME terrible
		}
		if( (c = malloc(sizeof(*c))) ){
			struct pci_cap *pcicap;
			uint32_t data;

			memset(c,0,sizeof(*c));
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
				c->pcie.gen = data & PCI_EXP_LNKSTA_SPEED;
				c->pcie.lanes_neg = (data & PCI_EXP_LNKSTA_WIDTH) >> 4u;
			}
			if((c->name = strdup(rbuf)) == NULL){
				// FIXME?
			}
		}
		pci_free_dev(pcidev);
		c->next = controllers;
		cur = controllers = c;
	}
	return cur;
}

const controller *get_controllers(void){
	return controllers; // FIXME hugely unsafe
}

static void
free_device(device *d){
	if(d){
		device *p;

		switch(d->layout){
			case LAYOUT_NONE:{
				break;
			}case LAYOUT_MDADM:{
				mdslave *md;

				while( (md = d->mddev.slaves) ){
					d->mddev.slaves = md->next;
					free(md->name);
					free(md);
				}
				free(d->mddev.level);
				break;
			}case LAYOUT_PARTITION:{
				free(d->partdev.pname);
				free(d->partdev.uuid);
				break;
			}case LAYOUT_ZPOOL:{
				break;
			}
		}
		while( (p = d->parts) ){
			d->parts = p->next;
			free_device(p);
		}
		free(d->mntops);
		free(d->mnttype);
		free(d->mnt);
		free(d->wwn);
		free(d->model);
		free(d->revision);
		free(d->pttable);
	}
}

static void
free_controller(controller *c){
	if(c){
		free(c->name);
	}
}

static void
free_devtable(void){
	controller *c;

	while( (c = controllers) ){
		device *d;

		while( (d = c->blockdevs) ){
			c->blockdevs = d->next;
			free_device(d);
			free(d);
		}
		controllers = c->next;
		// FIXME ugh! horrible!
		if(c->bus != BUS_VIRTUAL && c->bus != BUS_UNKNOWN){
			free_controller(c);
			free(c);
		}
	}
	close(sysfd); sysfd = -1;
	close(devfd); devfd = -1;
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

static device *
add_partition(device *d,const char *name,dev_t devno,uintmax_t sz){
	device *p;

	if( (p = malloc(sizeof(*p))) ){
		device **pre;

		memset(p,0,sizeof(*p));
		strcpy(p->name,name);
		for(pre = &d->parts ; *pre ; pre = &(*pre)->next){
			if(strcmp((*pre)->name,name) > 0){ // FIXME 0's no good
				break;
			}
		}
		p->devno = devno;
		p->next = *pre;
		p->size = sz;
		*pre = p;
	}
	return p;
}

// Pass a directory handle fd, and the bare name of the device
int explore_sysfs_node(int fd,const char *name,device *d){
	struct dirent *dire;
	unsigned long ul;
	unsigned b;
	int sdevfd;
	DIR *dir;

	// do *not* call closedir(3) on dir: doing so will close(2) fd.
	if((dir = fdopendir(fd)) == NULL){
		fprintf(stderr,"Couldn't get DIR * from fd %d for %s (%s?)\n",
				fd,name,strerror(errno));
		return -1;
	}
	if(get_sysfs_bool(fd,"queue/rotational",&b)){
		fprintf(stderr,"Couldn't determine rotation for %s (%s?)\n",name,strerror(errno));
	}else{
		d->blkdev.rotate = !!b;
	}
	if(get_sysfs_bool(fd,"removable",&b)){
		fprintf(stderr,"Couldn't determine removability for %s (%s?)\n",name,strerror(errno));
	}else{
		d->blkdev.removable = !!b;
	}
	if(get_sysfs_uint(fd,"size",&ul)){
		fprintf(stderr,"Couldn't determine size for %s (%s?)\n",name,strerror(errno));
	}else{
		d->size = ul;
	}
	// Check for "device" to determine if it's real or virtual
	if((sdevfd = openat(fd,"device",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
		d->blkdev.realdev = 1;
		if((d->model = get_sysfs_string(sdevfd,"model")) == NULL){
			fprintf(stderr,"Couldn't get a model for %s (%s?)\n",name,strerror(errno));
		}
		if((d->revision = get_sysfs_string(sdevfd,"rev")) == NULL){
			fprintf(stderr,"Couldn't get a revision for %s (%s?)\n",name,strerror(errno));
		}
		verbf("\tModel: %s revision %s\n",d->model,d->revision);
		close(sdevfd);
	}
	while(errno = 0, (dire = readdir(dir)) ){
		int subfd;

		if(dire->d_type == DT_DIR){
			if((subfd = openat(fd,dire->d_name,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
				dev_t devno;

				// Check for "md" to determine if it's an MDADM device
				if(strcmp(dire->d_name,"md") == 0){
					d->layout = LAYOUT_MDADM;
					if(explore_md_sysfs(d,subfd)){
						close(subfd);
						return -1;
					}
				}else if(sysfs_exist_p(subfd,"partition")){
					unsigned long sz;

					if(sysfs_devno(subfd,&devno)){
						close(subfd);
						return -1;
					}
					verbf("\tPartition at %s\n",dire->d_name);
					if(get_sysfs_uint(subfd,"size",&sz)){
						fprintf(stderr,"Couldn't determine size for %s (%s?)\n",
								dire->d_name,strerror(errno));
						sz = 0;
					}
					if(add_partition(d,dire->d_name,devno,sz) == NULL){
						close(subfd);
						return -1;
					}
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
static controller *
parse_bus_topology(const char *fn){
	unsigned long domain,bus,dev,func;
	const char *pci;

	if(strstr(fn,"/devices/virtual/")){
		return &virtual_bus;
	}
	if((pci = strstr(fn,"/devices/pci")) == NULL){
		return &unknown_bus;
	}
	pci += strlen("/devices/pci");
	if(parse_pci_busid(pci,&domain,&bus,&dev,&func)){
		fprintf(stderr,"Couldn't extract PCI address from %s\n",pci);
		return NULL;
	}
	return find_pcie_controller(domain,bus,dev,func);
}


static inline device *
create_new_device(const char *name){
	char buf[PATH_MAX] = "";
	controller *c;
	device *d,dd;
	int fd;

	memset(&dd,0,sizeof(dd));
	if(strlen(name) >= sizeof(d->name)){
		fprintf(stderr,"Name too long: %s\n",name);
		return NULL;
	}
	if(readlinkat(sysfd,name,buf,sizeof(buf)) < 0){
		fprintf(stderr,"Couldn't read link at %s%s (%s?)\n",
			SYSROOT,name,strerror(errno));
		return NULL;
	}else{
		verbf("%s -> %s\n",name,buf);
	}
	if((c = parse_bus_topology(buf)) == NULL){
		fprintf(stderr,"Couldn't get physical bus topology for %s\n",name);
		return NULL;
	}else{
		verbf("\tController: %s\n",c->name);
	}
	if((fd = openat(sysfd,buf,O_RDONLY|O_CLOEXEC)) < 0){
		fprintf(stderr,"Couldn't open link at %s%s (%s?)\n",
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
	if(dd.blkdev.realdev || (dd.layout == LAYOUT_MDADM)){
		char devbuf[PATH_MAX];
		blkid_parttable ptbl;
		blkid_topology tpr;
		blkid_partlist ppl;
		blkid_probe pr;
		int pars;
		int dfd;

		if(dd.blkdev.realdev){
			if((dfd = openat(devfd,name,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
				fprintf(stderr,"Couldn't open " DEVROOT "/%s (%s?)\n",name,strerror(errno));
				close(fd);
				free_device(&dd);
				return NULL;
			}
			if(sg_interrogate(&dd,dfd)){
				close(dfd);
				close(fd);
				free_device(&dd);
				return NULL;
			}
			close(dfd);
		}
		snprintf(devbuf,sizeof(devbuf),DEVROOT "/%s",name);
		// FIXME move all this to its own function
		if(probe_blkid_dev(devbuf,&pr) == 0){
			if( (ppl = blkid_probe_get_partitions(pr)) ){
				const char *pttable;
				device *p;

				if((ptbl = blkid_partlist_get_table(ppl)) == NULL){
					fprintf(stderr,"Couldn't probe partition table of %s (%s?)\n",name,strerror(errno));
					close(fd);
					free_device(&dd);
					blkid_free_probe(pr);
					return NULL;
				}
				pars = blkid_partlist_numof_partitions(ppl);
				pttable = blkid_parttable_get_type(ptbl);
				verbf("\t%d partition%s, table type %s\n",
						pars,pars == 1 ? "" : "s",
						pttable);
				if((dd.pttable = strdup(pttable)) == NULL){
					close(fd);
					free_device(&dd);
					blkid_free_probe(pr);
					return NULL;
				}
				for(p = dd.parts ; p ; p = p->next){
					blkid_partition part;

					part = blkid_partlist_devno_to_partition(ppl,p->devno);
					if(part){
						const char *uuid,*pname;

						uuid = blkid_partition_get_uuid(part);
						if(uuid){
							p->partdev.uuid = strdup(uuid);
						}
						pname = blkid_partition_get_name(part);
						if(pname){
							p->partdev.pname = strdup(pname);
						}
					}
				}
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
			dd.logsec = dd.physsec = 0;
			// FIXME errorchecking!
			dd.logsec = blkid_topology_get_logical_sector_size(tpr);
			dd.physsec = blkid_topology_get_physical_sector_size(tpr);
			if(dd.logsec || dd.physsec){
				device *p;

				verbf("\tLogical sector size: %uB Physical sector size: %uB\n",
						dd.logsec,dd.physsec);
				for(p = dd.parts ; p ; p = p->next){
					p->logsec = dd.logsec;
					p->physsec = dd.physsec;
				}
			}
			blkid_free_probe(pr);
		}else if(!dd.blkdev.removable || errno != ENOMEDIUM){
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
		d->next = c->blockdevs;
		c->blockdevs = d;
	}else{
		fprintf(stderr,"Couldn't look up %s (%s?)\n",name,strerror(errno));
		free_device(&dd);
	}
	return d;
}

// name must be an entry in /sys/device/block, and also one in /dev
device *lookup_device(const char *name){
	controller *c;
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
	for(c = controllers ; c ; c = c->next){
		for(d = c->blockdevs ; d ; d = d->next){
			device *p;

			if(strcmp(name,d->name) == 0){
				return d;
			}
			for(p = d->parts ; p ; p = p->next){
				if(strcmp(name,p->name) == 0){
					return p;
				}
			}
		}
	}
	return create_new_device(name);
}

// Must be an entry in /dev/disk/by-id/
device *lookup_id(const char *name){
	char path[PATH_MAX],buf[PATH_MAX];
	device *d;
	int rl;

	if(snprintf(path,sizeof(path),"/dev/disk/by-id/%s",name) >= (int)sizeof(path)){
		return NULL;
	}
	if((rl = readlink(path,buf,sizeof(buf))) < 0 || (unsigned)rl >= sizeof(buf)){
		return NULL;
	}
	buf[rl] = '\0';
	if( (d = lookup_device(buf)) ){
		if(strncasecmp(name,"wwn-0x",6) == 0){ // World Wide Name
			char *wwn;

			if((wwn = strdup(name + 6)) == NULL){
				return NULL;
			}
			free(d->wwn);
			d->wwn = wwn;
		}
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

typedef device *(*eventfxn)(const char *);

static inline int
watch_dir(int fd,const char *dfp,eventfxn fxn){
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

			if((dev = fxn(d->d_name)) == NULL){
				break;
			}
		}
		r = 0;
	}
	if(r == 0 && errno){
		fprintf(stderr,"Error reading %s (%s?)\n",dfp,strerror(errno));
		r = -1;
	}else if(r){
		fprintf(stderr,"Error processing %s\n",d->d_name);
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

int growlight_init(int argc,char * const *argv){
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
		goto err;
	}
	if((!(enc = nl_langinfo(CODESET))) || strcmp(enc,"UTF-8")){
		fprintf(stderr,"Output isn't UTF-8, aborting\n");
		goto err;
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
		goto err;
	}
	if(chdir(SYSROOT)){
		fprintf(stderr,"Couldn't cd to %s (%s?)\n",SYSROOT,strerror(errno));
		goto err;
	}
	if((sysfd = get_dir_fd(&sdir,SYSROOT)) < 0){
		goto err;
	}
	if((devfd = get_dir_fd(&sdir,DEVROOT)) < 0){
		goto err;
	}
	if((fd = inotify_fd()) < 0){
		goto err;
	}
	if(watch_dir(fd,SYSROOT,lookup_device)){
		goto err;
	}
	if(watch_dir(fd,DEVBYID,lookup_id)){
		goto err;
	}
	if(parse_mounts(MOUNTS)){
		goto err;
	}
	if(parse_swaps()){
		goto err;
	}
	return 0;

err:
	growlight_stop();
	return -1;
}

int growlight_stop(void){
	close_blkid();
	free_devtable();
	pci_cleanup(pciacc);
	return 0;
}

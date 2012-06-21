#define HAVE_IOCTL_IN_SYS_IOCTL_H
#include <assert.h>
#include <wchar.h>
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
#include <pci/pci.h>
#include <pthread.h>
#include <langinfo.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <scsi/scsi.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <pci/header.h>
#include <sys/inotify.h>
#include <libdevmapper.h>
#include <openssl/ssl.h>

#include "sg.h"
#include "mbr.h"
#include "zfs.h"
#include "swap.h"
#include "udev.h"
#include "mdadm.h"
#include "smart.h"
#include "sysfs.h"
#include "config.h"
#include "mounts.h"
#include "libblkid.h"
#include "growlight.h"

#define SYSROOT "/sys/block/"
#define MOUNTS	"/proc/mounts"
#define DEVROOT "/dev"
#define DEVBYID DEVROOT "/disk/by-id/"

static unsigned verbose;
static struct pci_access *pciacc;
static int sysfd = -1; // Hold a reference to SYSROOT
static int devfd = -1; // Hold a reference to DEVROOT
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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

void free_device(device *d){
	if(d){
		device *p;

		// free_targets() has references to, and frees, all the various
		// target structures. do not free them here!
		d->target = NULL;
		switch(d->layout){
			case LAYOUT_NONE:{
				free(d->blkdev.biossha1);
				free(d->blkdev.pttable);
				free(d->blkdev.serial);
				break;
			}case LAYOUT_MDADM:{
				mdslave *md;

				while( (md = d->mddev.slaves) ){
					d->mddev.slaves = md->next;
					free(md->name);
					free(md);
				}
				free(d->mddev.level);
				free(d->mddev.uuid);
				free(d->mddev.mdname);
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
		free(d->uuid);
		free(d->mnt);
		free(d->wwn);
		free(d->model);
		free(d->revision);
	}
}

static void
clobber_device(device *d){
	free_device(d);
	free(d);
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
			clobber_device(d);
		}
		controllers = c->next;
		// FIXME ugh! horrible!
		if(c->bus != BUS_VIRTUAL && c->bus != BUS_UNKNOWN){
			free_controller(c);
			free(c);
		}
	}
}

static device *
add_partition(device *d,const char *name,dev_t devno,unsigned pnum,uintmax_t sz){
	device *p;

	if(strlen(name) >= sizeof(p->name)){
		fprintf(stderr,"Bad name: %s\n",name);
		return NULL;
	}
	if(!pnum){
		fprintf(stderr,"Can't work with partition number %u\n",pnum);
		return NULL;
	}
	if( (p = malloc(sizeof(*p))) ){
		device **pre;

		memset(p,0,sizeof(*p));
		p->layout = LAYOUT_PARTITION;
		p->swapprio = SWAP_INVALID;
		strcpy(p->name,name);
		// FIXME ought sort by disk order not partition number
		for(pre = &d->parts ; *pre ; pre = &(*pre)->next){
			if((*pre)->partdev.pnumber >= pnum){
				break;
			}
		}
		p->partdev.pnumber = pnum;
		p->partdev.parent = d;
		p->devno = devno;
		p->size = sz;
		p->next = *pre;
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
	if(sysfs_exist_p(fd,"partition")){
		fprintf(stderr,"We were passed a partition (%s)!\n",name);
		return -1;
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
		verbf("\tModel: %s revision %s S/N %s\n",
				d->model ? d->model : "n/a",
				d->revision ? d->revision : "n/a",
				d->blkdev.serial ? d->blkdev.serial : "n/a");
		close(sdevfd);
		if(get_sysfs_bool(fd,"queue/rotational",&b)){
			fprintf(stderr,"Couldn't determine rotation for %s (%s?)\n",name,strerror(errno));
		}else{
			d->blkdev.rotate = !!b;
		}
	}
	while(errno = 0, (dire = readdir(dir)) ){
		int subfd;

		if(dire->d_type == DT_DIR){
			if((subfd = openat(fd,dire->d_name,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
				dev_t devno;

				// Check for "md" to determine if it's an MDADM device
				if(strcmp(dire->d_name,"md") == 0){
					d->layout = LAYOUT_MDADM;
					memset(&d->mddev,0,sizeof(d->mddev));
					if(explore_md_sysfs(d,subfd)){
						close(subfd);
						return -1;
					}
				}else if(sysfs_exist_p(subfd,"partition")){
					unsigned long sz,pnum;

					if(sysfs_devno(subfd,&devno)){
						close(subfd);
						return -1;
					}
					if(get_sysfs_uint(subfd,"partition",&pnum)){
						fprintf(stderr,"Couldn't determine pnum for %s (%s?)\n",
								dire->d_name,strerror(errno));
						pnum = 0;
					}
					verbf("\tPartition %lu at %s\n",pnum,dire->d_name);
					if(get_sysfs_uint(subfd,"size",&sz)){
						fprintf(stderr,"Couldn't determine size for %s (%s?)\n",
								dire->d_name,strerror(errno));
						sz = 0;
					}
					if(add_partition(d,dire->d_name,devno,pnum,sz) == NULL){
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

// Used by systems which don't properly populate sysfs (*cough* zfs *cough*)
void add_new_virtual_blockdev(device *d){
	d->next = virtual_bus.blockdevs;
	virtual_bus.blockdevs = d;
}

static device *
create_new_device(const char *name){
	char buf[PATH_MAX] = "";
	controller *c;
	device *d;
	int fd;

	if(strlen(name) >= sizeof(d->name)){
		fprintf(stderr,"Bad name: %s\n",name);
		return NULL;
	}
	if((d = malloc(sizeof(*d))) == NULL){
		fprintf(stderr,"Couldn't allocate space for %s\n",name);
		return NULL;
	}
	memset(d,0,sizeof(*d));
	strcpy(d->name,name);
	d->swapprio = SWAP_INVALID;
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
		clobber_device(d);
		return NULL;
	}
	if(explore_sysfs_node(fd,name,d)){
		close(fd);
		clobber_device(d);
		return NULL;
	}
	if(close(fd)){
		fprintf(stderr,"Couldn't close fd %d (%s?)\n",fd,strerror(errno));
		clobber_device(d);
		return NULL;
	}
	if((d->layout == LAYOUT_NONE && d->blkdev.realdev) ||
			(d->layout == LAYOUT_MDADM)){
		char devbuf[PATH_MAX];
		blkid_parttable ptbl;
		blkid_topology tpr;
		blkid_partlist ppl;
		blkid_probe pr;
		int pars;
		int dfd;

		if(d->layout == LAYOUT_NONE && d->blkdev.realdev){
			if((dfd = openat(devfd,name,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
				fprintf(stderr,"Couldn't open " DEVROOT "/%s (%s?)\n",name,strerror(errno));
				clobber_device(d);
				return NULL;
			}
			if(sg_interrogate(d,dfd)){
				close(dfd);
				clobber_device(d);
				return NULL;
			}
			probe_smart(d);
			if((d->blkdev.biossha1 = malloc(20)) == NULL){
				fprintf(stderr,"Couldn't alloc SHA1 buf (%s?)\n",strerror(errno));
				clobber_device(d);
				return NULL;
			}
			if(mbrsha1(dfd,d->blkdev.biossha1)){
				if(!d->blkdev.removable){
					fprintf(stderr,"Warning: Couldn't read MBR for %s\n",name);
				}
				free(d->blkdev.biossha1);
				d->blkdev.biossha1 = NULL;
			}
			close(dfd);
		}
		snprintf(devbuf,sizeof(devbuf),DEVROOT "/%s",name);
		// FIXME move all this to its own function
		if(probe_blkid_superblock(devbuf,&pr,d) == 0){
			if( (ppl = blkid_probe_get_partitions(pr)) ){
				const char *pttable;
				device *p;

				if((ptbl = blkid_partlist_get_table(ppl)) == NULL){
					fprintf(stderr,"Couldn't probe partition table of %s (%s?)\n",name,strerror(errno));
					clobber_device(d);
					blkid_free_probe(pr);
					return NULL;
				}
				pars = blkid_partlist_numof_partitions(ppl);
				pttable = blkid_parttable_get_type(ptbl);
				verbf("\t%d partition%s, table type %s\n",
						pars,pars == 1 ? "" : "s",
						pttable);
				if((d->blkdev.pttable = strdup(pttable)) == NULL){
					clobber_device(d);
					blkid_free_probe(pr);
					return NULL;
				}
				for(p = d->parts ; p ; p = p->next){
					blkid_partition part;

					part = blkid_partlist_devno_to_partition(ppl,p->devno);
					if(part){
						unsigned long long flags;

						flags = blkid_partition_get_flags(part);
						// FIXME need find UEFI EPS partitions
						if(strcmp(pttable,"gpt") == 0){
							p->partdev.partrole = PARTROLE_GPT;
							d->blkdev.biosboot = !zerombrp(d->blkdev.biossha1);
							// FIXME verify bootable flag?
						}else if(blkid_partition_is_extended(part)){
							p->partdev.partrole = PARTROLE_EXTENDED;
						}else if(blkid_partition_is_logical(part)){
							p->partdev.partrole = PARTROLE_LOGICAL;
						}else if(blkid_partition_is_primary(part)){
							p->partdev.partrole = PARTROLE_PRIMARY;
							d->blkdev.biosboot = !zerombrp(d->blkdev.biossha1);
							// FIXME verify bootable flag?
						}
// BIOS boot flag byte ought not be set to anything but 0 unless we're on a
// primary partition and doing BIOS+MBR booting, in which case it must be 0x80.
						if((flags & 0xff) != 0){
							if(p->partdev.partrole != PARTROLE_PRIMARY || ((flags & 0xffu) != 0x80)){
								fprintf(stderr,"Warning: BIOS+MBR boot byte was %02llx on %s\n",
										flags & 0xffu,p->name);
								clobber_device(d);
								blkid_free_probe(pr);
								return NULL;
							}
						}
						p->partdev.flags = flags;
						if(probe_blkid_superblock(p->name,NULL,p)){
							clobber_device(d);
							blkid_free_probe(pr);
							return NULL;
						}
					}
				}
			}else{
				device *p;

				verbf("\tNo partition table\n");
				while( (p = d->parts) ){
					fprintf(stderr,"Eliminating malingering partition %s\n",p->name);
					d->parts = p->next;
					clobber_device(p);
				}
			}
			if((tpr = blkid_probe_get_topology(pr)) == NULL){
				fprintf(stderr,"Couldn't probe topology of %s (%s?)\n",name,strerror(errno));
				clobber_device(d);
				blkid_free_probe(pr);
				return NULL;
			}
			d->logsec = d->physsec = 0;
			// FIXME errorchecking!
			d->logsec = blkid_topology_get_logical_sector_size(tpr);
			d->physsec = blkid_topology_get_physical_sector_size(tpr);
			if(d->logsec || d->physsec){
				device *p;

				verbf("\tLogical sector size: %uB Physical sector size: %uB\n",
						d->logsec,d->physsec);
				for(p = d->parts ; p ; p = p->next){
					p->logsec = d->logsec;
					p->physsec = d->physsec;
				}
			}
			blkid_free_probe(pr);
		}else if(!d->blkdev.removable || errno != ENOMEDIUM){
			fprintf(stderr,"Couldn't probe %s (%s?)\n",name,strerror(errno));
			clobber_device(d);
			return NULL;
		}else{
			verbf("\tDevice is unloaded/inaccessible\n");
		}
	}
	d->next = c->blockdevs;
	c->blockdevs = d;
	return d;
}

// name must be an entry in /sys/class/block, and also one in /dev
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

static int
scan_device(const char *name){
	return lookup_device(name) ? 0 : -1;
}

// Must be an entry in /dev/disk/by-id/
static int
lookup_id(const char *name){
	char path[PATH_MAX],buf[PATH_MAX];
	device *d;
	int rl;

	if(snprintf(path,sizeof(path),"/dev/disk/by-id/%s",name) >= (int)sizeof(path)){
		return -1;
	}
	if((rl = readlink(path,buf,sizeof(buf))) < 0 || (unsigned)rl >= sizeof(buf)){
		return -1;
	}
	buf[rl] = '\0';
	if( (d = lookup_device(buf)) ){
		if(strncasecmp(name,"wwn-0x",6) == 0){ // World Wide Name
			char *wwn;

			if((wwn = strdup(name + 6)) == NULL){
				return -1;
			}
			free(d->wwn);
			d->wwn = wwn;
		}
	}else{
		fprintf(stderr,"Warning: couldn't trace down %s\n",path);
	}
	return 0;
}

static inline int
inotify_fd(void){
	int fd;

	if((fd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC)) < 0){
		fprintf(stderr,"Coudln't get inotify fd (%s?)\n",strerror(errno));
	}
	return fd;
}

typedef int (*eventfxn)(const char *);

static inline int
watch_dir(int fd,const char *dfp,eventfxn fxn){
	struct dirent *d;
	DIR *dir;
	int wfd,r;
	int dfd;

	if(fd >= 0){
		wfd = inotify_add_watch(fd,dfp,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
		if(wfd < 0){
			fprintf(stderr,"Coudln't inotify on %s (%s?)\n",dfp,strerror(errno));
			return -1;
		}else{
			verbf("Watching %s on fd %d\n",dfp,wfd);
		}
	}
	r = 0;
	if((dir = opendir(dfp)) == NULL){
		fprintf(stderr,"Coudln't open %s (%s?)\n",dfp,strerror(errno));
		if(fd >= 0){ inotify_rm_watch(fd,wfd); }
		return -1;
	}
	if((dfd = dirfd(dir)) < 0){
		fprintf(stderr,"Coudln't get fd on %s (%s?)\n",dfp,strerror(errno));
		if(fd >= 0){ inotify_rm_watch(fd,wfd); }
		closedir(dir);
		return -1;
	}
	while( errno = 0, (d = readdir(dir)) ){
		r = -1;
		if(d->d_type == DT_LNK){
			if(fxn(d->d_name)){
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

static pthread_t eventtid;

struct event_marshal {
	int efd;		// epoll fd
	int ifd;		// inotify fd
	int ufd;		// udev_monitor fd
};

static void *
event_posix_thread(void *unsafe){
	const struct event_marshal *em = unsafe;
	struct epoll_event events[128];
	int e,r;

	do{
		e = epoll_wait(em->efd,events,sizeof(events) / sizeof(*events),-1);
		for(r = 0 ; r < e ; ++r){
			if(events[r].data.fd == em->ifd){
				char buf[BUFSIZ];
				ssize_t s;

				assert(events[r].events == EPOLLIN);
				while((s = read(em->ifd,buf,sizeof(buf))) > 0){
					const struct inotify_event *in;
					unsigned idx = 0;

					if(s - idx >= (ptrdiff_t)sizeof(*in)){
						in = (struct inotify_event *)(buf + idx);
						idx += sizeof(*in);
						if(in->len){
							verbf("Event on %s\n",in->name);
						}
						// FIXME do something with it
					}
				}
				if(s && errno != EAGAIN && errno != EWOULDBLOCK){
					fprintf(stderr,"Error reading inotify event on %d (%s?)\n",
							em->ifd,strerror(errno));
				}
			}else if(events[r].data.fd == em->ufd){
				udev_event();
			}else{
				fprintf(stderr,"Unknown fd %d saw event\n",events[r].data.fd);
			}
		}
	}while(e >= 0);
	fprintf(stderr,"Error processing event queue (%s?)\n",strerror(errno));
	return NULL;
}

static int
event_thread(int fd,int ufd){
	struct event_marshal *em;
	struct epoll_event ev;
	int r;

	memset(&ev,0,sizeof(ev));
	ev.events = EPOLLIN | EPOLLRDHUP;
	if((em = malloc(sizeof(*em))) == NULL){
		fprintf(stderr,"Couldn't create event marshal (%s?)\n",strerror(errno));
		return -1;
	}
	if((em->efd = epoll_create1(EPOLL_CLOEXEC)) < 0){
		fprintf(stderr,"Couldn't create epoll (%s?)\n",strerror(errno));
		free(em);
		return -1;
	}
	ev.data.fd = fd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,fd,&ev)){
		fprintf(stderr,"Couldn't add %d to epoll (%s?)\n",fd,strerror(errno));
		close(em->efd);
		free(em);
		return -1;
	}
	ev.data.fd = ufd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,ufd,&ev)){
		fprintf(stderr,"Couldn't add %d to epoll (%s?)\n",ufd,strerror(errno));
		close(em->efd);
		free(em);
		return -1;
	}
	em->ifd = fd;
	em->ufd = ufd;
	if( (r = pthread_create(&eventtid,NULL,event_posix_thread,em)) ){
		fprintf(stderr,"Couldn't create event thread (%s?)\n",strerror(r));
		close(em->efd);
		free(em);
		return -1;
	}
	return 0;
}

static int
kill_event_thread(void){
	int r = 0,rr;

	if( (rr = pthread_cancel(eventtid)) ){
		fprintf(stderr,"Couldn't cancel event thread (%s?)\n",strerror(rr));
		r |= -1;
	}
	if( (rr = pthread_join(eventtid,NULL)) ){
		fprintf(stderr,"Couldn't join event thread (%s?)\n",strerror(rr));
		r |= -1;
	}
	r |= shutdown_udev();
	return r;
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
	int fd,opt,longidx,udevfd;
	char buf[BUFSIZ];
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
	SSL_library_init();
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
	dm_get_library_version(buf,sizeof(buf));
	printf("%s %s\nlibblkid %s, libpci 0x%x, libdm %s\n",PACKAGE,
			PACKAGE_VERSION,BLKID_VERSION,PCI_LIB_VERSION,buf);
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
	if(init_zfs_support()){
		goto err;
	}
	if(watch_dir(fd,SYSROOT,scan_device)){
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
	if((udevfd = monitor_udev()) < 0){
		goto err;
	}
	if(event_thread(fd,udevfd)){
		goto err;
	}
	return 0;

err:
	growlight_stop();
	return -1;
}

int growlight_stop(void){
	int r = 0;

	r |= kill_event_thread();
	r |= close_blkid();
	free_devtable();
	pci_cleanup(pciacc);
	r |= stop_zfs_support();
	close(sysfd); sysfd = -1;
	close(devfd); devfd = -1;
	return r;
}

int reset_blockdev(device *d){
	char buf[PATH_MAX];
	unsigned t;
	int fd;

	if(snprintf(buf,sizeof(buf),SYSROOT"/%s/device/rescan",d->name) >= (int)sizeof(buf)){
		fprintf(stderr,"Name too long: %s\n",d->name);
		return -1;
	}
	if(write_sysfs(buf,"1\n")){
		return -1;
	}
	printf("Wrote '1' to %s\n",buf);
	if((fd = openat(devfd,d->name,O_RDONLY|O_CLOEXEC)) < 0){
		return -1;
	}
	sync();
	// The ioctl can fail for a number of reasons, usually because the
	// work's still being done. Give it a try or two.
	for(t = 0 ; t < 3 ; ++t){
		if(ioctl(fd,BLKRRPART,NULL) == 0){
			close(fd);
			printf("Updated kernel partition table\n");
			sync();
			return 0;
		}
		fprintf(stderr,"Error calling BLKRRPART on %s (%s?), retrying in 5s...\n",buf,strerror(errno));
		sleep(5);
	}
	close(fd);
	return -1;
}

int lock_growlight(void){
	int r;

	if( (r = pthread_mutex_lock(&lock)) ){
		fprintf(stderr,"Error locking mutex (%s?)\n",strerror(errno));
	}
	return r;
}

int unlock_growlight(void){
	int r;

	if( (r = pthread_mutex_unlock(&lock)) ){
		fprintf(stderr,"Error unlocking mutex (%s?)\n",strerror(errno));
	}
	return r;
}

static int
rescan(device *d){
	device *tmp;

	if((tmp = create_new_device(d->name)) == NULL){
		return -1;
	}
	free_device(d);
	*d = *tmp;
	free(tmp);
	return 0;
}

int rescan_device(const char *name){
	device **lnk;
	controller *c;
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
		for(lnk = &c->blockdevs ; *lnk ; lnk = &(*lnk)->next){
			device **plnk;

			// FIXME need to update mount/swap/target tables!
			if(strcmp(name,(*lnk)->name) == 0){
				// FIXME this method will leave us with broken
				// partition links
				device *d = *lnk;
				*lnk = d->next;
				free_device(d);
				return 0;
			}
			for(plnk = &(*lnk)->parts ; *plnk ; plnk = &(*plnk)->next){
				if(strcmp(name,(*plnk)->name) == 0){
					if(rescan(*plnk)){
						device *p = *plnk;
						*plnk = p->next;
						free_device(p);
						return 0;
					}
				}
			}
		}
	}
	if(create_new_device(name) == NULL){
		return -1;
	}
	return 0;
}

int rescan_devices(void){
	int ret = 0;

	// FIXME this is slightly overkill. also, it eliminates any mappings.
	free_devtable();
	ret |= watch_dir(-1,SYSROOT,scan_device);
	ret |= watch_dir(-1,DEVBYID,lookup_id);
	ret |= parse_mounts(MOUNTS);
	ret |= parse_swaps();
	return ret;
}

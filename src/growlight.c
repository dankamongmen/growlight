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
#include <linux/fs.h>
#include <sys/stat.h>
#include <scsi/scsi.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <pciaccess.h>
#include <pci/header.h>
#include <sys/inotify.h>
#include <openssl/ssl.h>
#include <libdevmapper.h>
#include <gnu/libc-version.h>

#include "fs.h"
#include "sg.h"
#include "mbr.h"
#include "zfs.h"
#include "swap.h"
#include "udev.h"
#include "mdadm.h"
#include "popen.h"
#include "smart.h"
#include "sysfs.h"
#include "config.h"
#include "mounts.h"
#include "target.h"
#include "libblkid.h"
#include "growlight.h"

#define SYSROOT "/sys/class/block/"
#define SWAPS "/proc/swaps"
#define MOUNTS	"/proc/mounts"
#define FILESYSTEMS	"/proc/filesystems"
#define DEVROOT "/dev"
#define DEVBYID DEVROOT "/disk/by-id/"

unsigned verbose = 0;
unsigned finalized = 0;

int sysfd = -1; // Hold a reference to SYSROOT, for openat() etc
int devfd = -1; // Hold a reference to DEVROOT

static unsigned usepci;
static const glightui *gui;
static struct pci_access *pciacc;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned thrcount;
static pthread_mutex_t barrier = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cond = PTHREAD_COND_INITIALIZER;

// Global state for a growlight instance
typedef struct devtable {
	controller *controllers;
	device *virtual_blockdevs;
} devtable;

static controller virtual_bus = {
	.name = "Virtual devices",
	.next = NULL,
	.ident = "virtual",
	.bus = BUS_VIRTUAL,
};

static controller *controllers = &virtual_bus;

static device *create_new_device(const char *,int);
static device *create_new_device_inner(const char *,int);

// Diagnostics. We keep the last MAXIMUM_LOG_ENTRIES records around for clients
// to examine at their leisure, ala dmesg(1).
static unsigned rblast;
static logent logs[MAXIMUM_LOG_ENTRIES];
static pthread_mutex_t loglock = PTHREAD_MUTEX_INITIALIZER;

static void
add_log(const char *fmt,va_list vac){
	va_list vacc;
	char *b;
	int len;

	va_copy(vacc,vac);
	assert(pthread_mutex_lock(&loglock) == 0);
	if(++rblast == sizeof(logs) / sizeof(*logs)){
		rblast = 0;
	}
	// FIXME reuse the entry!
	len = vsnprintf(NULL,0,fmt,vac);
	if( (b = malloc(len + 1)) ){
		logs[rblast].when = time(NULL);
		if(logs[rblast].msg){
			free(logs[rblast].msg);
		}
		logs[rblast].msg = b;
		vsnprintf(b,len + 1,fmt,vacc);
	}
	assert(pthread_mutex_unlock(&loglock) == 0);
	va_end(vacc);
}

int get_logs(unsigned n,logent *cplogs){
	unsigned idx = 0;
	unsigned rb;

	if(n == 0 || n > MAXIMUM_LOG_ENTRIES){
		return -1;
	}
	assert(pthread_mutex_lock(&loglock) == 0);
	rb = rblast;
	while(logs[rb].msg){
		if((cplogs[idx].msg = strdup(logs[rb].msg)) == NULL){
			while(idx){
				free(cplogs[--idx].msg);
			}
			return -1;
		}
		cplogs[idx].when = logs[rb].when;
		if(rb-- == 0){
			rb = sizeof(logs) / sizeof(*logs) - 1;
		}
		if(++idx == n){
			break; // got all requested
		}
	}
	assert(pthread_mutex_unlock(&loglock) == 0);
	if(idx < n){
		cplogs[idx].msg = NULL;
	}
	return idx;
}

static void
push_devtable(devtable *dt){
	dt->controllers = controllers;
	dt->virtual_blockdevs = virtual_bus.blockdevs;
	virtual_bus.blockdevs = NULL;
	controllers = &virtual_bus;
}


void diag(const char *fmt,...){
	va_list vac,ap;

	va_start(ap,fmt);
	va_copy(vac,ap);
	gui->vdiag(fmt,ap);
	add_log(fmt,vac);
	va_end(vac);
}

void verbf(const char *fmt,...){
	va_list ap;

	va_start(ap,fmt);
	if(verbose){
		va_list vac;

		va_copy(vac,ap);
		gui->vdiag(fmt,vac);
		va_end(vac);
	}
	add_log(fmt,ap);
	va_end(ap);
}

static void
free_controller(controller *c){
	if(c){
		free(c->biosver);
		free(c->fwver);
		free(c->driver);
		free(c->ident);
		free(c->sysfs);
		free(c->name);
	}
}

static inline int
usbmodulep(const char *driver){
	if(strcmp(driver,"xhci_hcd") == 0){
		return TRANSPORT_USB3;
	}else if(strcmp(driver,"ehci_hcd") == 0){
		return TRANSPORT_USB2;
	}else if(strcmp(driver,"uhci_hcd") == 0){
		return TRANSPORT_USB;
	}else if(strcmp(driver,"ohci_hcd") == 0){
		return TRANSPORT_USB;
	}
	return 0;
}

static controller *
find_pcie_controller(unsigned domain,unsigned bus,unsigned dev,unsigned func,
			char *module,char *sysfs){
	controller *c;

	for(c = controllers ; c ; c = c->next){
		if(c->bus != BUS_PCIe){
			continue;
		}
		if(c->pcie.domain != domain || c->pcie.bus != bus){
			continue;
		}
		if(c->pcie.dev != dev || c->pcie.func != func){
			continue;
		}
		break;
	}
	if(c == NULL){
		const controller *idc;
		unsigned devno = 0;
		controller **pre;

		for(idc = controllers ; idc ; idc = idc->next){
			if(idc->driver && strcmp(idc->driver,module) == 0){
				++devno;
			}
		}
		if((c = malloc(sizeof(*c))) == NULL){
			return NULL;
		}else{
			size_t len = strlen(module) + 7; // FIXME sketchy

			memset(c,0,sizeof(*c));
			if( (c->ident = malloc(len)) ){
				if(snprintf(c->ident,len,"%s-%u",module,devno) >= (int)len){
					free(c);
					return NULL;
				}
			}else{
				free(c);
				return NULL;
			}
		}
		c->transport = usbmodulep(module);
		c->sysfs = sysfs;
		c->driver = module;
		c->bus = BUS_PCIe;
		c->pcie.domain = domain;
		c->pcie.bus = bus;
		c->pcie.dev = dev;
		c->pcie.func = func;
		if(usepci){
			struct pci_cap *pcicap;
			const char *vend,*model;
			struct pci_device *pci;
			struct pci_dev *pcidev;
			char buf[BUFSIZ];
			uint32_t data;

			if((pci = pci_device_find_by_slot(domain,bus,dev,func)) == NULL){
				diag("Couldn't look up PCIe device\n");
				free_controller(c);
				free(c);
				return NULL;
			}
			if((pcidev = pci_get_dev(pciacc,domain,bus,dev,func)) == NULL){
				diag("Couldn't look up PCIe device\n");
				free_controller(c);
				free(c);
				return NULL;
			}
			assert(pci_fill_info(pcidev,PCI_FILL_IDENT|PCI_FILL_IRQ|PCI_FILL_BASES|PCI_FILL_ROM_BASE|
							PCI_FILL_CAPS|PCI_FILL_EXT_CAPS|
							PCI_FILL_SIZES|PCI_FILL_RESCAN));
			vend = pci_device_get_vendor_name(pci);
			model = pci_device_get_device_name(pci);
			snprintf(buf,sizeof(buf),"%s %s",
					vend ? vend : "Unknown vendor",
					model ? model : "unknown model");
			if((c->name = strdup(buf)) == NULL){
				pci_free_dev(pcidev);
				free_controller(c);
				free(c);
				return NULL;
			}
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
				if(c->pcie.gen == 1){
					c->bandwidth = 250u * 8u * 1000000ull;
				}else if(c->pcie.gen == 2){
					c->bandwidth = 500u * 8u * 1000000ull;
				}else if(c->pcie.gen == 3){
					c->bandwidth = 1000u * 8u * 1000000ull;
				}else if(c->pcie.gen == 4){
					c->bandwidth = 2000u * 8u * 1000000ull;
				}
				c->bandwidth *= c->pcie.lanes_neg;
			}
			pci_free_dev(pcidev);
		}
		for(pre = &controllers ; *pre ; pre = &(*pre)->next){
			int r = (*pre)->ident ? strcmp(c->ident,(*pre)->ident) : -1;

			if(r < 0){
				break;
			}
		}
		c->next = *pre;
		*pre = c;
		c->uistate = gui->adapter_event(c,NULL);
	}else{
		free(module);
		free(sysfs);
	}
	return c;
}

const controller *get_controllers(void){
	return controllers; // FIXME hugely unsafe
}

static void
free_device(device *d){
	if(d){
		device *p;

		if(d->c && d->c->uistate && d->uistate){
			gui->block_free(d->c->uistate,d->uistate);
		}
		free_mntentry(d->target);
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
		free(d->label);
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
free_devtable(devtable *dt){
	controller *c;
	device *d;

	while( (c = dt->controllers) ){
		device *d;

		if(c->bus == BUS_VIRTUAL){
			break;
		}
		while( (d = c->blockdevs) ){
			c->blockdevs = d->next;
			clobber_device(d);
		}
		dt->controllers = c->next;
		if(c->uistate){
			gui->adapter_free(c->uistate);
		}
		free_controller(c);
		free(c);
	}
	while( (d = dt->virtual_blockdevs) ){
		dt->virtual_blockdevs = d->next;
		clobber_device(d);
	}
}

static device *
add_partition(device *d,const char *name,dev_t devno,unsigned pnum,
				uint64_t fsect,uintmax_t sz){
	device *p;

	if(strlen(name) >= sizeof(p->name)){
		diag("Bad name: %s\n",name);
		return NULL;
	}
	if(!pnum){
		diag("Can't work with partition number %u\n",pnum);
		return NULL;
	}
	if(!sz){
		diag("Can't work with 0-sector partition\n");
		return NULL;
	}
	if( (p = malloc(sizeof(*p))) ){
		device **pre;

		memset(p,0,sizeof(*p));
		p->layout = LAYOUT_PARTITION;
		p->swapprio = SWAP_INVALID;
		strcpy(p->name,name);
		for(pre = &d->parts ; *pre ; pre = &(*pre)->next){
			if((*pre)->partdev.fsector >= fsect){
				break;
			}
		}
		p->partdev.fsector = fsect;
		p->partdev.lsector = fsect + sz - 1;
		p->partdev.pnumber = pnum;
		p->partdev.parent = d;
		p->devno = devno;
		p->size = sz;
		p->next = *pre;
		p->c = d->c;
		*pre = p;
	}
	return p;
}

// Pass a directory handle fd, and the bare name of the device
// Return -1 on error, 0 on success, 1 if the device is a partition, and we
// successfully look up the containing disk (in which case lookup_device()
// ought be rerun to acquire a reference).
static int
explore_sysfs_node_inner(DIR *dir,int fd,const char *name,device *d,int recurse){
	struct dirent *dire;
	unsigned long ul;
	unsigned b;
	int sdevfd;

	if(sysfs_exist_p(fd,"partition")){
		char buf[PATH_MAX],*dev;
		int r;

		if(recurse){
			verbf("Not recursing on partition %s\n",name);
			return -1;
		}
		if((r = readlinkat(sysfd,name,buf,sizeof(buf))) < 0){
			diag("Couldn't read link at %s%s (%s?)\n",
				SYSROOT,name,strerror(errno));
			return -1;
		}
		buf[r] = '\0';
		if((dev = strrchr(buf,'/')) == NULL){
			diag("Bad link: "SYSROOT"%s->%s\n",name,buf);
			return -1;
		}
		*dev++ = '\0';
		if(strcmp(dev,name)){
			diag("Invalid link: "SYSROOT"%s->%s/%s\n",name,buf,dev);
			return -1;
		}
		if((dev = strrchr(buf,'/')) == NULL){
			diag("Bad toplink: "SYSROOT"%s->%s\n",name,buf);
			return -1;
		}
		++dev;
		if(create_new_device_inner(dev,0) == NULL){
			diag("Couldn't get disk: "SYSROOT"%s->%s/%s\n",name,buf,dev);
			return -1;
		}
		return 1;
	}
	if(sysfs_exist_p(fd,"loop")){
		if((d->model = get_sysfs_string(fd,"loop/backing_file")) == NULL){
			diag("Couldn't get backing file: %s\n",name);
			return -1;
		}
	}
	if(get_sysfs_bool(fd,"removable",&b)){
		diag("Couldn't determine removability for %s (%s?)\n",name,strerror(errno));
	}else{
		d->blkdev.removable = !!b;
	}
	if(get_sysfs_uint(fd,"size",&ul)){
		diag("Couldn't determine size for %s (%s?)\n",name,strerror(errno));
	}else{
		d->size = ul;
	}
	// Check for "device" to determine if it's real or virtual
	if((sdevfd = openat(fd,"device",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY)) > 0){
		d->blkdev.realdev = 1;
		if((d->model = get_sysfs_string(sdevfd,"model")) == NULL){
			diag("Couldn't get a model for %s (%s?)\n",name,strerror(errno));
		}
		if((d->revision = get_sysfs_string(sdevfd,"rev")) == NULL){
			diag("Couldn't get a revision for %s (%s?)\n",name,strerror(errno));
		}
		verbf("\tModel: %s revision %s S/N %s\n",
				d->model ? d->model : "n/a",
				d->revision ? d->revision : "n/a",
				d->blkdev.serial ? d->blkdev.serial : "n/a");
		close(sdevfd);
		if(get_sysfs_bool(fd,"queue/rotational",&b)){
			diag("Couldn't determine rotation for %s (%s?)\n",name,strerror(errno));
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
					unsigned long sz,pnum,fsect;

					if(sysfs_devno(subfd,&devno)){
						close(subfd);
						return -1;
					}
					if(get_sysfs_uint(subfd,"partition",&pnum)){
						diag("Couldn't determine pnum for %s (%s?)\n",
								dire->d_name,strerror(errno));
						pnum = 0;
					}
					verbf("\tPartition %lu at %s\n",pnum,dire->d_name);
					if(get_sysfs_uint(subfd,"start",&fsect)){
						diag("Couldn't determine first sector for %s (%s?)\n",
								dire->d_name,strerror(errno));
						sz = 0;
					}
					if(get_sysfs_uint(subfd,"size",&sz)){
						diag("Couldn't determine size for %s (%s?)\n",
								dire->d_name,strerror(errno));
						sz = 0;
					}
					if(add_partition(d,dire->d_name,devno,pnum,fsect,sz) == NULL){
						close(subfd);
						return -1;
					}
				}
				close(subfd);
			}else{
				diag("Couldn't open directory at %s for %s (%s?)\n",
						dire->d_name,name,strerror(errno));
				return -1;
			}
		}
	}
	if(errno){
		diag("Error walking sysfs:%s (%s?)\n",name,strerror(errno));
		return -1;
	}
	return 0;
}

static int
explore_sysfs_node(int fd,const char *name,device *d,int recurse){
	DIR *dir;
	int r;

	if((dir = fdopendir(fd)) == NULL){
		diag("Couldn't get DIR * from fd %d for %s (%s?)\n",
				fd,name,strerror(errno));
		return -1;
	}
	r = explore_sysfs_node_inner(dir,fd,name,d,recurse);
	closedir(dir); // close(2)s fd
	return r;
}

// Returns the sysfs node for the device
static char *
parse_pci_busid(const char *busid,unsigned long *domain,unsigned long *bus,
                                unsigned long *dev,unsigned long *func,
				char **module){
	char buf[PATH_MAX];
        const char *cur;
        char *e,*sysfs;
	int dir,r;

        cur = busid + strlen("/sys/devices/pci");
        // FIXME clean this cut-and-paste crap up
        if(*cur == '-'){ // strtoul() admits leading negations
                return NULL;
        }
        if(strtoul(cur,&e,16) == ULONG_MAX){
                return NULL;
        }
        if(*e != ':'){
                return NULL;
        }
        cur = e + 1;
        if(*cur == '-'){ // strtoul() admits leading negations
                return NULL;
        }
        if(strtoul(cur,&e,16) == ULONG_MAX){
                return NULL;
        }
        if(*e != '/'){
                return NULL;
        }
	*domain = *bus = *dev = *func = 0; // FIXME purge
	// FIXME hack! we ought check to see if the PCI device we just
	// resolved is a bridge, and if so, keep going. instead, check
	// whatever comes next. no bueno!
	while(cur = e + 1, !isalpha(*cur)){
		if(*cur == '-'){ // strtoul() admits leading negations
			return NULL;
		}
		if((*domain = strtoul(cur,&e,16)) == ULONG_MAX){
			return NULL;
		}
		if(*e != ':'){
			return NULL;
		}
		cur = e + 1;
		if(*cur == '-'){ // strtoul() admits leading negations
			return NULL;
		}
		if((*bus = strtoul(cur,&e,16)) == ULONG_MAX){
			return NULL;
		}
		if(*e != ':'){
			return NULL;
		}
		cur = e + 1;
		if(*cur == '-'){ // strtoul() admits leading negations
			return NULL;
		}
		if((*dev = strtoul(cur,&e,16)) == ULONG_MAX){
			return NULL;
		}
		if(*e != '.'){
			return NULL;
		}
		cur = e + 1;
		if(*cur == '-'){ // strtoul() admits leading negations
			return NULL;
		}
		if((*func = strtoul(cur,&e,16)) == ULONG_MAX){
			return NULL;
		}
		if(*e != '/'){
			return NULL;
		}
	}
	if((sysfs = strndup(busid,cur - busid)) == NULL){
		return NULL;
	}
	if((dir = open(sysfs,O_RDONLY|O_CLOEXEC)) < 0){
		diag("Couldn't open %s (%s?)\n",sysfs,strerror(errno));
		free(sysfs);
		return NULL;
	}
	if((r = readlinkat(dir,"driver/module",buf,sizeof(buf))) < 0){
		diag("Couldn't read link at %.*s/driver/module (%s?)\n",(int)(cur - busid),busid,strerror(errno));
		free(sysfs);
		return NULL;
	}
	buf[r] = '\0';
	close(dir);
	if((e = strrchr(buf,'/')) == NULL || !*++e){
		diag("Bad module name: %s\n",buf);
		free(sysfs);
		return NULL;
	}
	if((*module = strdup(e)) == NULL){
		free(sysfs);
		return NULL;
	}
        return sysfs;
}

// Takes the sysfs link as read when dereferencing /sys/block/*. Only works
// for virtual/PCI currently.
static controller *
parse_bus_topology(const char *fn){
	unsigned long domain,bus,dev,func;
	char buf[PATH_MAX],*module,*sysfs;
	controller *c;

	if(strstr(fn,"/devices/virtual/")){
		return &virtual_bus;
	}
	if(realpath(fn,buf) == NULL){
		diag("Couldn't canonicalize %s\n",fn);
		return NULL;
	}
	if((sysfs = parse_pci_busid(buf,&domain,&bus,&dev,&func,&module)) == NULL){
		verbf("Couldn't extract PCI address from %s\n",buf);
		return &virtual_bus;
	}
	if((c = find_pcie_controller(domain,bus,dev,func,module,sysfs)) == NULL){
		free(module);
		free(sysfs);
		return NULL;
	}
	return c;
}

// Used by systems which don't properly populate sysfs (*cough* zfs *cough*)
void add_new_virtual_blockdev(device *d){
	assert(lock_growlight() == 0);
		d->c = &virtual_bus;
		d->next = virtual_bus.blockdevs;
		virtual_bus.blockdevs = d;
		d->uistate = gui->block_event(d,d->uistate);
	assert(unlock_growlight() == 0);
}

static device *
create_new_device_inner(const char *name,int recurse){
	char buf[PATH_MAX] = "";
	device *d;
	int fd,r;

	diag("Discovering %s...\n",name);
	if(strlen(name) >= sizeof(d->name)){
		diag("Bad name: %s\n",name);
		return NULL;
	}
	if((d = malloc(sizeof(*d))) == NULL){
		diag("Couldn't allocate space for %s\n",name);
		return NULL;
	}
	memset(d,0,sizeof(*d));
	strcpy(d->name,name);
	d->swapprio = SWAP_INVALID;
	if(strlen(name) >= sizeof(d->name)){
		diag("Name too long: %s\n",name);
		return NULL;
	}
	if(readlinkat(sysfd,name,buf,sizeof(buf)) < 0){
		diag("Couldn't read link at %s%s (%s?)\n",
			SYSROOT,name,strerror(errno));
		return NULL;
	}else{
		verbf("%s -> %s\n",name,buf);
	}
	if((d->c = parse_bus_topology(buf)) == NULL){
		diag("Couldn't get physical bus topology for %s\n",name);
		return NULL;
	}else{
		verbf("\tController: %s\n",d->c->name);
	}
	if((fd = openat(sysfd,buf,O_RDONLY|O_CLOEXEC)) < 0){
		diag("Couldn't open link at %s%s (%s?)\n",
			SYSROOT,buf,strerror(errno));
		clobber_device(d);
		return NULL;
	}
	// close(2)s fd
	if((r = explore_sysfs_node(fd,name,d,recurse)) < 0){
		clobber_device(d);
		return NULL;
	}else if(r){
		// The device ought exist now. Don't continue trying to create
		// a new one, but instead look up the one that now exists.
		clobber_device(d);
		return lookup_device(name);
	}
	if(d->c == &virtual_bus && d->layout == LAYOUT_NONE){
		d->blkdev.realdev = 0;
	}
	// Allow d->model to run the checks on validly-filebacked loop devices
	if((d->layout == LAYOUT_NONE && (d->blkdev.realdev || d->model))
			|| (d->layout == LAYOUT_MDADM)){
		char devbuf[PATH_MAX];
		blkid_parttable ptbl;
		blkid_partlist ppl;
		blkid_probe pr;
		int pars;
		int dfd;

		if(d->layout == LAYOUT_NONE && d->blkdev.realdev){
			if((dfd = openat(devfd,name,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
				diag("Couldn't open " DEVROOT "/%s (%s?)\n",name,strerror(errno));
				clobber_device(d);
				return NULL;
			}
			if(d->c->transport == TRANSPORT_ATA){
				if(sg_interrogate(d,dfd)){
					close(dfd);
					clobber_device(d);
					return NULL;
				}
				probe_smart(d);
			}else if(d->c->transport == TRANSPORT_USB){
				d->blkdev.transport = SERIAL_USB;
			}else if(d->c->transport == TRANSPORT_USB2){
				d->blkdev.transport = SERIAL_USB2;
			}else if(d->c->transport == TRANSPORT_USB3){
				d->blkdev.transport = SERIAL_USB3;
			}
			if(!d->blkdev.removable){
				if((d->blkdev.biossha1 = malloc(20)) == NULL){
					diag("Couldn't alloc SHA1 buf (%s?)\n",strerror(errno));
					clobber_device(d);
					return NULL;
				}
				if(mbrsha1(dfd,d->blkdev.biossha1)){
					diag("Warning: Couldn't read MBR for %s\n",name);
					free(d->blkdev.biossha1);
					d->blkdev.biossha1 = NULL;
				}
			}else{
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
					diag("Couldn't probe partition table of %s (%s?)\n",name,strerror(errno));
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
								diag("Warning: BIOS+MBR boot byte was %02llx on %s\n",
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
					diag("Eliminating malingering partition %s\n",p->name);
					d->parts = p->next;
					clobber_device(p);
				}
			}
			if(d->logsec || d->physsec){
				device *p;

				verbf("\tLogical sector size: %uB Physical sector size: %uB\n",
						d->logsec,d->physsec);
				d->size *= d->logsec;
				for(p = d->parts ; p ; p = p->next){
					p->logsec = d->logsec;
					p->physsec = d->physsec;
					p->size *= p->logsec;
				}
			}
			blkid_free_probe(pr);
		}else if(!d->blkdev.removable || errno != ENOMEDIUM){
			diag("Couldn't probe %s (%s?)\n",name,strerror(errno));
			clobber_device(d);
			return NULL;
		}else{
			verbf("\tDevice is unloaded/inaccessible\n");
		}
	}
	d->next = d->c->blockdevs;
	d->c->blockdevs = d;
	d->uistate = gui->block_event(d,d->uistate);
	diag("Done with %s!\n",name);
	return d;
}

static device *
create_new_device(const char *name,int recurse){
	char cwd[PATH_MAX + 1];
	device *d;

	if(getcwd(cwd,sizeof(cwd)) == NULL){
		diag("Couldn't get working directory (%s?)\n",strerror(errno));
		return NULL;
	}
	if(chdir(SYSROOT)){
		diag("Couldn't cd to %s (%s?)\n",SYSROOT,strerror(errno));
		return NULL;
	}
	d = create_new_device_inner(name,recurse);
	if(chdir(cwd)){
		diag("Warning: couldn't return to %s (%s?)\n",cwd,strerror(errno));
	}
	return d;
}

controller *lookup_controller(const char *name){
	controller *c;

	for(c = controllers ; c ; c = c->next){
		if(strcmp(name,c->ident) == 0){
			break;
		}
	}
	if(!c){
		diag("Couldn't find device \"%s\"\n",name);
	}
	return c;
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
	return create_new_device(name,1);
}

static void *
scan_device(void *name){
	char cwd[PATH_MAX + 1];
	device *d;
	int sig;

	if(getcwd(cwd,sizeof(cwd)) == NULL){
		diag("Couldn't get working directory (%s?)\n",strerror(errno));
		pthread_mutex_unlock(&barrier);
		return NULL;
	}
	if(chdir(SYSROOT)){
		diag("Couldn't cd to %s (%s?)\n",SYSROOT,strerror(errno));
		pthread_mutex_unlock(&barrier);
		return NULL;
	}
	assert(lock_growlight() == 0);
	d = name ? lookup_device(name) : NULL;
	assert(unlock_growlight() == 0);
	assert(pthread_mutex_lock(&barrier) == 0);
	sig = --thrcount == 0;
	if(sig){
		pthread_cond_signal(&barrier_cond);
	}
	assert(pthread_mutex_unlock(&barrier) == 0);
	if(chdir(cwd)){
		diag("Warning: couldn't return to %s (%s?)\n",cwd,strerror(errno));
	}
	free(name);
	return d;
}

// Must be an entry in /dev/disk/by-id/
static device *
lookup_id_inner(const char *name){
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
	}else{
		diag("Warning: couldn't trace down %s\n",path);
	}
	return d;
}

static void *
lookup_id(void *uname){
	const char *name = uname;
	char cwd[PATH_MAX + 1];
	device *d;

	if(getcwd(cwd,sizeof(cwd)) == NULL){
		diag("Couldn't get working directory (%s?)\n",strerror(errno));
		pthread_mutex_unlock(&barrier);
		return NULL;
	}
	if(chdir(SYSROOT)){
		diag("Couldn't cd to %s (%s?)\n",SYSROOT,strerror(errno));
		pthread_mutex_unlock(&barrier);
		return NULL;
	}
	assert(lock_growlight() == 0);
	d = name ? lookup_id_inner(name) : NULL;
	assert(unlock_growlight() == 0);
	assert(pthread_mutex_lock(&barrier) == 0);
	if(--thrcount == 0){
		pthread_cond_signal(&barrier_cond);
	}
	assert(pthread_mutex_unlock(&barrier) == 0);
	if(chdir(cwd)){
		diag("Warning: couldn't return to %s (%s?)\n",cwd,strerror(errno));
	}
	free(uname);
	return d;
}

static inline int
inotify_fd(void){
	int fd;

	if((fd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC)) < 0){
		diag("Coudln't get inotify fd (%s?)\n",strerror(errno));
	}
	return fd;
}

typedef void *(*eventfxn)(void *);

static inline int
watch_dir(int fd,const char *dfp,eventfxn fxn){
	pthread_attr_t attr;
	struct dirent *d;
	int wfd,r,dfd;
	DIR *dir;

	if( (r = pthread_attr_init(&attr)) ||
		(r = pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED))){
		diag("Couldn't set threads detachable (%s?)\n",strerror(errno));
	}
	pthread_mutex_lock(&barrier);
	assert(thrcount == 0);
	pthread_mutex_unlock(&barrier);
	if(fd >= 0){
		wfd = inotify_add_watch(fd,dfp,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
		if(wfd < 0){
			diag("Coudln't inotify on %s (%s?)\n",dfp,strerror(errno));
			return -1;
		}else{
			verbf("Watching %s on fd %d\n",dfp,wfd);
		}
	}
	r = 0;
	if((dir = opendir(dfp)) == NULL){
		diag("Coudln't open %s (%s?)\n",dfp,strerror(errno));
		if(fd >= 0){ inotify_rm_watch(fd,wfd); }
		pthread_attr_destroy(&attr);
		return -1;
	}
	if((dfd = dirfd(dir)) < 0){
		diag("Coudln't get fd on %s (%s?)\n",dfp,strerror(errno));
		if(fd >= 0){ inotify_rm_watch(fd,wfd); }
		closedir(dir);
		pthread_attr_destroy(&attr);
		return -1;
	}
	while( errno = 0, (d = readdir(dir)) ){
		pthread_t tid;
		r = -1;
		if(d->d_type == DT_LNK){
			pthread_mutex_lock(&barrier);
			++thrcount;
			pthread_mutex_unlock(&barrier);
			if( (r = pthread_create(&tid,NULL,fxn,strdup(d->d_name))) ){
				diag("Couldn't create thread (%s?)\n",strerror(errno));
				pthread_mutex_lock(&barrier);
				--thrcount;
				pthread_mutex_unlock(&barrier);
				break;
			}
		}
		r = 0;
	}
	if(r == 0 && errno){
		diag("Error reading %s (%s?)\n",dfp,strerror(errno));
		r = -1;
	}else if(r){
		diag("Error processing %s\n",d->d_name);
		r = -1;
	}
	closedir(dir);
	pthread_mutex_lock(&barrier);
	while(thrcount){
		verbf("Waiting on %u devices...\n",thrcount);
		pthread_cond_wait(&barrier_cond,&barrier);
	}
	verbf("Device discovery completed\n");
	pthread_mutex_unlock(&barrier);
	pthread_attr_destroy(&attr);
	return r;
}

static void
version(const char *name,int status){
	if(gui->fatal){
		gui->fatal("%s version %s\n",name,VERSION);
	}else{
		FILE *fp = status == EXIT_SUCCESS ? stdout : stderr;

		fprintf(fp,"%s version %s\n",name,VERSION);
	}
	exit(status);
}

static void
usage(const char *name,int status){
	if(gui->fatal){
		gui->fatal("usage: %s [ -h|--help ] [ -v|--verbose ] [ -V|--version ]\n"
				"\t\t[ -t|--target path ]\n",name);
	}else{
		FILE *fp = status != EXIT_SUCCESS ? stderr : stdout;

		fprintf(fp,"usage: %s [ -h|--help ] [ -v|--verbose ] [ -V|--version ]\n"
				"\t\t[ -t|--target path ]\n",name);
	}
	exit(status);
}

static int
get_dir_fd(const char *root){
	int fd;

	if((fd = open(root,O_RDONLY|O_CLOEXEC|O_DIRECTORY)) < 0){
		diag("Couldn't get dirfd at %s (%s?)\n",root,strerror(errno));
	}
	return fd;
}

static int
glight_pci_init(void){
	if(pci_system_init()){
		return -1;
	}
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
	int mfd;		// /proc/mounts fd
	int sfd;		// /proc/swaps fd
	int ffd;		// /proc/filesystems fd
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
					diag("Error reading inotify event on %d (%s?)\n",
							em->ifd,strerror(errno));
				}
			}else if(events[r].data.fd == em->ufd){
				udev_event();
			}else if(events[r].data.fd == em->mfd){
				lock_growlight();
				diag("Reparsing %s...\n",MOUNTS);
				clear_mounts(controllers);
				parse_mounts(gui,MOUNTS);
				unlock_growlight();
			}else if(events[r].data.fd == em->sfd){
				lock_growlight();
				diag("Reparsing %s...\n",SWAPS);
				parse_swaps(gui,SWAPS);
				unlock_growlight();
			}else if(events[r].data.fd == em->ffd){
				lock_growlight();
				diag("Reparsing %s...\n",FILESYSTEMS);
				parse_filesystems(gui,FILESYSTEMS);
				unlock_growlight();
			}else{
				diag("Unknown fd %d saw event\n",events[r].data.fd);
			}
		}
	}while(e >= 0);
	diag("Error processing event queue (%s?)\n",strerror(errno));
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
		diag("Couldn't create event marshal (%s?)\n",strerror(errno));
		return -1;
	}
	if((em->efd = epoll_create1(EPOLL_CLOEXEC)) < 0){
		diag("Couldn't create epoll (%s?)\n",strerror(errno));
		free(em);
		return -1;
	}
	ev.data.fd = fd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,fd,&ev)){
		diag("Couldn't add %d to epoll (%s?)\n",fd,strerror(errno));
		close(em->efd);
		free(em);
		return -1;
	}
	ev.data.fd = ufd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,ufd,&ev)){
		diag("Couldn't add %d to epoll (%s?)\n",ufd,strerror(errno));
		close(em->efd);
		free(em);
		return -1;
	}
	em->ifd = fd;
	em->ufd = ufd;
	if((em->mfd = open(MOUNTS,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		close(em->efd);
		free(em);
		return -1;
	}
	if((em->sfd = open(SWAPS,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		close(em->mfd);
		close(em->efd);
		free(em);
		return -1;
	}
	if((em->ffd = open(FILESYSTEMS,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		close(em->sfd);
		close(em->mfd);
		close(em->efd);
		free(em);
		return -1;
	}
	// /proc/* always returns readable. On change they return EPOLLERR.
	ev.events = EPOLLRDHUP;
	ev.data.fd = em->ffd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,em->ffd,&ev)){
		diag("Couldn't add %d to epoll (%s?)\n",em->ffd,strerror(errno));
		close(em->ffd);
		close(em->sfd);
		close(em->mfd);
		close(em->efd);
		free(em);
		return -1;
	}
	ev.data.fd = em->sfd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,em->sfd,&ev)){
		diag("Couldn't add %d to epoll (%s?)\n",em->sfd,strerror(errno));
		close(em->ffd);
		close(em->sfd);
		close(em->mfd);
		close(em->efd);
		free(em);
		return -1;
	}
	ev.data.fd = em->mfd;
	if(epoll_ctl(em->efd,EPOLL_CTL_ADD,em->mfd,&ev)){
		diag("Couldn't add %d to epoll (%s?)\n",em->mfd,strerror(errno));
		close(em->ffd);
		close(em->sfd);
		close(em->mfd);
		close(em->efd);
		free(em);
		return -1;
	}
	if( (r = pthread_create(&eventtid,NULL,event_posix_thread,em)) ){
		diag("Couldn't create event thread (%s?)\n",strerror(r));
		close(em->ffd);
		close(em->sfd);
		close(em->mfd);
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
		diag("Couldn't cancel event thread (%s?)\n",strerror(rr));
		r |= -1;
	}
	if( (rr = pthread_join(eventtid,NULL)) ){
		diag("Couldn't join event thread (%s?)\n",strerror(rr));
		r |= -1;
	}
	r |= shutdown_udev();
	return r;
}

static void
init_special_adapters(void){
	controller *c;

	for(c = controllers ; c ; c = c->next){
		c->uistate = gui->adapter_event(c,NULL);
	}
}

int growlight_init(int argc,char * const *argv,const glightui *ui){
	static const struct option ops[] = {
		{
			.name = "help",
			.has_arg = 0,
			.flag = NULL,
			.val = 'h',
		},{
			.name = "target",
			.has_arg = 2,
			.flag = NULL,
			.val = 't',
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

	gui = ui;
	if(setlocale(LC_ALL,"") == NULL){
		diag("Couldn't set locale (%s?)\n",strerror(errno));
		goto err;
	}
	SSL_library_init();
	opterr = 0; // disallow getopt(3) diagnostics to stderr
	while((opt = getopt_long(argc,argv,":ht:vV",ops,&longidx)) >= 0){
		switch(opt){
		case 'h':{
			usage(argv[0],EXIT_SUCCESS);
			break;
		}case 't':{
			if(growlight_target){
				diag("Error: defined --target twice (%s, %s)\n",
						growlight_target,optarg);
				usage(argv[0],EXIT_FAILURE);
			}else if(optarg == NULL){
				diag("-t|--target requires an argument\n");
				usage(argv[0],EXIT_FAILURE);
			}else{
				if(set_target(optarg)){
					return EXIT_FAILURE;
				}
			}
			break;
		}case 'v':{
			verbose = 1;
			break;
		}case 'V':{
			version(argv[0],EXIT_SUCCESS);
			break;
		}case ':':{
			diag("Option requires argument: '%c'\n",optopt);
			usage(argv[0],EXIT_FAILURE);
			break;
		}case '?':{
			diag("Unknown option: '%c'\n",optopt);
			usage(argv[0],EXIT_FAILURE);
			break;
		}default:{
			diag("Misuse of option: '%c'\n",optopt);
			usage(argv[0],EXIT_FAILURE);
			break;
		} }
	}
	dm_get_library_version(buf,sizeof(buf));
	verbf("%s %s\nlibblkid %s, libpci 0x%x, libdm %s, glibc %s %s\n",PACKAGE,
			PACKAGE_VERSION,BLKID_VERSION,PCI_LIB_VERSION,buf,
			gnu_get_libc_version(),gnu_get_libc_release());
	if(glight_pci_init()){
		diag("Couldn't init libpciaccess (%s?)\n",strerror(errno));
	}else{
		usepci = 1;
	}
	if((sysfd = get_dir_fd(SYSROOT)) < 0){
		goto err;
	}
	if((devfd = get_dir_fd(DEVROOT)) < 0){
		goto err;
	}
	if((fd = inotify_fd()) < 0){
		goto err;
	}
	init_special_adapters();
	if(init_zfs_support(gui)){
		goto err;
	}
	if(watch_dir(fd,SYSROOT,scan_device)){
		goto err;
	}
	if(watch_dir(fd,DEVBYID,lookup_id)){
		diag("Couldn't monitor %s; won't have WWNs\n",DEVBYID);
	}
	if(parse_filesystems(gui,FILESYSTEMS)){
		goto err;
	}
	if(parse_mounts(gui,MOUNTS)){
		goto err;
	}
	if(parse_swaps(gui,SWAPS)){
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
	devtable dt;
	int r = 0;

	r |= kill_event_thread();
	r |= close_blkid();
	push_devtable(&dt);
	free_devtable(&dt);
	if(usepci){
		pci_cleanup(pciacc);
	}
	usepci = 0;
	r |= stop_zfs_support();
	close(sysfd); sysfd = -1;
	close(devfd); devfd = -1;
	if(growlight_target){
		return (finalized && !r) ? 0 : -1;
	}
	return r;
}

int rescan_controller(controller *c){
	char buf[PATH_MAX];

	if(snprintf(buf,sizeof(buf),"%s/device/rescan",c->sysfs) >= (int)sizeof(buf)){
		diag("Name too long: %s\n",c->sysfs);
		return -1;
	}
	if(write_sysfs(buf,"1\n")){
		return -1;
	}
	diag("Wrote '1' to %s\n",buf);
	sync();
	return 0;
}

int reset_controller(controller *c){
	char buf[PATH_MAX];

	if(snprintf(buf,sizeof(buf),"%s/device/reset",c->sysfs) >= (int)sizeof(buf)){
		diag("Name too long: %s\n",c->sysfs);
		return -1;
	}
	if(write_sysfs(buf,"1\n")){
		return -1;
	}
	diag("Wrote '1' to %s\n",buf);
	sync();
	return 0;
}

int benchmark_blockdev(const device *d){
	char buf[PATH_MAX];

	if(snprintf(buf,sizeof(buf),"hdparm -t /dev/%s",d->name) >= (int)sizeof(buf)){
		diag("Name too long: %s\n",d->name);
		return -1;
	}
	if(popen_drain(buf)){
		return -1;
	}
	return 0;
}

int rescan_blockdev(device *d){
	char buf[PATH_MAX];
	unsigned t;
	int fd;

	if(snprintf(buf,sizeof(buf),SYSROOT"/%s/device/rescan",d->name) >= (int)sizeof(buf)){
		diag("Name too long: %s\n",d->name);
		return -1;
	}
	if(write_sysfs(buf,"1\n")){
		return -1;
	}
	diag("Wrote '1' to %s\n",buf);
	if((fd = openat(devfd,d->name,O_RDONLY|O_CLOEXEC)) < 0){
		return -1;
	}
	sync();
	// The ioctl can fail for a number of reasons, usually because the
	// work's still being done. Give it a try or two.
	for(t = 0 ; t < 2 ; ++t){
		if(ioctl(fd,BLKRRPART,NULL) == 0){
			goto success;
		}
		diag("Error calling BLKRRPART on "DEVROOT"/%s (%s?), retrying in 5s...\n",d->name,strerror(errno));
		sleep(5);
	}
	if(ioctl(fd,BLKRRPART,NULL) == 0){
		goto success;
	}
	diag("Couldn't send BLKRRPART (%s?), aborting\n",strerror(errno));
	close(fd);
	return -1;

success:
	close(fd);
	diag("Updated kernel partition table\n");
	sync();
	return 0;
}

int lock_growlight(void){
	int r;

	if( (r = pthread_mutex_lock(&lock)) ){
		diag("Error locking mutex (%s?)\n",strerror(errno));
	}
	return r;
}

int unlock_growlight(void){
	int r;

	if( (r = pthread_mutex_unlock(&lock)) ){
		diag("Error unlocking mutex (%s?)\n",strerror(errno));
	}
	return r;
}

static int
rescan(device *d){
	device *tmp;

	if((tmp = create_new_device(d->name,0)) == NULL){
		return -1;
	}
	tmp->target = d->target;
	d->target = NULL;
	free_device(d);
	*d = *tmp;
	free(tmp);
	return 0;
}

int reset_adapters(void){
	diag("Not yet implemented.");
	return -1;
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
				// FIXME this method might leave us with broken
				// partition links...?
				if(rescan(*lnk)){
					device *d = *lnk;
					*lnk = d->next;
					free_device(d);
					return 0;
				}
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
					return 0;
				}
			}
		}
	}
	if(create_new_device(name,0) == NULL){
		return -1;
	}
	return 0;
}

static int
devices_match_p(const device *d,const device *dd){
	unsigned mismatch = 0,match = 0;

	strcmp(d->name,dd->name) ? ++mismatch : ++match;
	(!d->uuid && !dd->uuid) ? ++match :
		((d->uuid && !dd->uuid) || (!d->uuid && dd->uuid) ||
			strcmp(d->uuid,dd->uuid)) ? ++mismatch : ++match;
	(!d->label && !dd->label) ? ++match :
		((d->label && !dd->label) || (!d->label && dd->label) ||
			strcmp(d->label,dd->label)) ? ++mismatch : ++match;
	return (mismatch && !match) ? 0 : (mismatch && match) ? -1 : 1;
}

// Must match in all four ways: UUID, label, device name, and bus path. Any
// partial match is cause to fail the search, since it represents ambiguity.
static device *
match_device(const device *d){
	controller *c;

	for(c = controllers ; c ; c = c->next){
		device *cd;

		for(cd = c->blockdevs ; cd ; cd = cd->next){
			int r;

			if(d->layout == LAYOUT_PARTITION){
				device *dp;

				for(dp = cd->parts ; dp ; dp = dp->next){
					if((r = devices_match_p(d,dp)) > 0){
						return dp;
					}
				}
			}else{
				if((r = devices_match_p(d,cd)) > 0){
					return cd;
				}
			}
			if(r < 0){
				return NULL;
			}
		}
	}
	return NULL;
}

int rescan_devices(void){
	const controller *c;
	device *d,*t,*p;
	int ret = 0;
	devtable dt;

	push_devtable(&dt);
	ret |= scan_zpools(gui);
	ret |= watch_dir(-1,SYSROOT,scan_device);
	ret |= watch_dir(-1,DEVBYID,lookup_id);
	ret |= parse_mounts(gui,MOUNTS);
	ret |= parse_swaps(gui,SWAPS);
	// Preserve any defined mappings, if possible. For a mapping to be
	// preserved, the device must still exist, with the same parameters,
	// and all containing mappings must also be preserved.
	for(c = dt.controllers ; c ; c = c->next){
		for(d = c->blockdevs ; d ; d = d->next){
			if(d->target){
				if( (t = match_device(d)) ){
					t->target = d->target;
					d->target = NULL;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->target){
					if( (t = match_device(p)) ){
						t->target = p->target;
						p->target = NULL;
					}
				}
			}
		}
	}
	for(d = dt.virtual_blockdevs ; d ; d = d->next){
		if(d->target){
			if( (t = match_device(d)) ){
				t->target = d->target;
				d->target = NULL;
			}
		}
		for(p = d->parts ; p ; p = p->next){
			if(p->target){
				if( (t = match_device(p)) ){
					t->target = p->target;
					p->target = NULL;
				}
			}
		}
	}
	free_devtable(&dt);
	return ret;
}

int prepare_bios_boot(device *d){
	if(d->layout != LAYOUT_PARTITION){
		diag("Must boot from a partition\n");
		return -1;
	}
	if(d->target == NULL){
		diag("%s is not mapped as a target filesystem\n",d->name);
		return -1;
	}
	if(strcmp(d->target->path,"/")){
		diag("%s is not mapped as the target root (%s)\n",d->name,d->target->path);
		return -1;
	}
	if(d->partdev.partrole == PARTROLE_GPT || d->partdev.partrole == PARTROLE_PRIMARY){
		if(!(d->partdev.flags & 0x80u)){
			diag("%s is not marked as Active (bootable, 0x80)\n",d->name);
			return -1;
		}
		if(vspopen_drain("grub-install --boot-directory=%s/boot/grub --no-floppy /dev/%s",d->mnt,d->name)){
			return -1;
		}
	}else{
		diag("BIOS boots from GPT or MSDOS 'Primary' partitions only\n");
		return -1;
	}
	return 0;
}

int prepare_uefi_boot(device *d){
	if(d->layout != LAYOUT_PARTITION){
		diag("Must boot from a partition\n");
		return -1;
	}
	if(d->partdev.partrole != PARTROLE_GPT){
		diag("UEFI boots from GPT partitions only\n");
		return -1;
	}
	// FIXME ensure the partition is a viable ESP
	// FIXME ensure kernel is in ESP
	// FIXME prepare protective MBR
	// FIXME install grub-efi to ESP
	// FIXME point grub-efi at kernel
	diag("FIXME %s not yet implemented\n",d->name);
	return -1;
}

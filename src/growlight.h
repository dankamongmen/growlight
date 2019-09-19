#ifndef GROWLIGHT_GROWLIGHT
#define GROWLIGHT_GROWLIGHT

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <wchar.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#include "gpt.h"
#include "stats.h"
#include "mounts.h"
#include "ptypes.h"
#include "target.h"
#include "config.h"

extern unsigned verbose;
extern unsigned finalized;
void diag(const char *,...) __attribute__ ((format (printf,1,2)));
void verbf(const char *,...) __attribute__ ((format (printf,1,2)));

extern int sysfd,devfd;

#define GUIDSTRLEN 36	// 16 2-char hex pairs with 4 hyphens
#define FSLABELSIZ 17	// 16 chars + null terminator

	// This isn't really suitable for use as a library to programs beyond
	// growlight. Not yet, in any case.

struct controller;

// Growlight's callback-based UI
typedef struct growlight_ui {
	void (*vdiag)(const char *,va_list); // free-form diagnostics
	
	// bulk information requiring receipt
	void (*boxinfo)(const char *,...);

	// Called on a new adapter, or when one changes
	void *(*adapter_event)(struct controller *,void *);

	// Called for a new blockdev, or when one changes
	void *(*block_event)(struct device *,void *);

	// Controller state
	void (*adapter_free)(void *);

	// Controller state followed by block state
	void (*block_free)(void *,void *);
} glightui;

const glightui *get_glightui(void);

int growlight_init(int,char * const *,const glightui *,int *);
int growlight_stop(void);

struct device;

typedef struct mdslave {
	char *name;			// Name of component
	//struct device *component;	// Block device holding component of
					//  mdadm device
	struct mdslave *next;		// Next in this md device
} mdslave;

typedef enum {
	TRANSPORT_UNKNOWN,
	PARALLEL_ATA,
	SERIAL_UNKNOWN,
	SERIAL_ATA8,
	SERIAL_ATAI,
	SERIAL_ATAII,
	SERIAL_ATAIII,
	SERIAL_USB,
	SERIAL_USB2,
	SERIAL_USB3,
	DIRECT_NVME,
	AGGREGATE_UNKNOWN,
	AGGREGATE_MIXED,
} transport_e;

typedef enum {
	RWVERIFY_UNSUPPORTED,
	RWVERIFY_SUPPORTED_OFF,
	RWVERIFY_SUPPORTED_ON,
} rwverify_status;

typedef struct {
	unsigned count;
	char **list;
} stringlist;

// An (non-link) entry in the device hierarchy, representing a block-type
// device (this includes hardware block devices, virtual block devices, and
// partitions). A partition corresponds to one and only one block device (which
// of course might represent multiple devices, or maybe just a file mounted via
// loopback). A partition or an unpartitioned block device can have a
// filesystem signature "mnttype". This filesystem's size might be different
// from the partition size (it is not safe to mount if the size is larger than
// the partition). The filesystem may be mounted at zero or more places, with
// different options each time.
typedef struct device {
	char name[NAME_MAX + 1];	// Entry in /dev or /sys/block
	struct device *next;		// next block device on this controller
	// FIXME model/revision should not be in partition
	char *model,*revision;		// Arbitrary UTF-8 strings
	// FIXME add by-label, and by-uuid links? handle multiple by-* links?
	char *bypath;			// Alias in /dev/disks/by-path/
	char *byid;			// Alias in /dev/disks/by-id/
	uintmax_t size;			// Size in bytes of device
	// If the filesystem is not mounted, but is found, only mnttype and
	// mntsize will be set from among mnt, mntops, mntsize and mnttype.
	// uuid and label can likewise only be set if mnttype is set.
	char *uuid;			// *Filesystem* UUID
	char *label;			// *Filesystem* label
	char *mnttype;			// Type of mount (can be "swap")
	uintmax_t mntsize;		// Filesystem size in bytes
	stringlist mnt;			// Active mount points
	stringlist mntops;		// Corresponding mount options
	// Ranges from 0 to 32565, 0 highest priority. For our purposes, we
	// also use -1, indicating "unused", and -2, indicating "not swap".
	enum {
		SWAP_INVALID = -2,
		SWAP_INACTIVE = -1,
		SWAP_MAXPRIO = 0,
		SWAP_MINPRIO = 65535,
	} swapprio;		// Priority as a swap device
	unsigned logsec;	// Logical sector size in bytes
	unsigned physsec;	// Physical sector size in bytes
	struct controller *c;
	char *sched;		// I/O scheduler (can be NULL)
	unsigned roflag;	// Read-only flag (hdparm -r, blockdev --getro)
	int slave;		// Number of owning devices
	union { // keyed off value of "layout" enum below
		struct {
			transport_e transport;
			unsigned realdev: 1;	// Is itself a real block device
			unsigned removable: 1;	// Removable media
			unsigned wcache: 1;	// Write cache enabled
			unsigned biosboot: 1;	// Non-zero bytes in MBR code area
			unsigned rwverify: 2;	// Read-Write-Verify:
						//  0: unsupported
						//  1: supported, off
						//  2: supported, on
						//  (see rwverify_status above)
			unsigned unloaded: 1;	// No media loaded
			void *biossha1;		// SHA1 of first 440 bytes
			char *pttable;		// Partition table type (can be NULL)
			char *serial;		// Serial number (can be NULL)
			char *wwn;		// World Wide Name
			int32_t rotation;	// Rotation rate:
						// 0 == unknown, -1 == SSD

			// The following two are relative to the static
			// partition table metainfo, not created partitions.
			uint64_t first_usable;	// First usable logical sector
			uint64_t last_usable;	// Last usable logical sector

			int smart;		// -1 for no support, otherwise
						//  SkSmartOverall enum values
			uint64_t celsius;	// Last-polled temperature
		} blkdev;
		struct { // mdadm (MDRAID)
			unsigned long disks;	// RAID disks in md
			char *level;		// RAID level
			mdslave *slaves;	// RAID components
			char *uuid;
			char *mdname;
			transport_e transport;
			unsigned long degraded;	// number of missing devices
			char *pttable;		// Partition table type (can be NULL)
			unsigned resync;
			uintmax_t stride;	// Chunk (stride in ext4 talk)
			unsigned swidth;	// Stripe width (non-parity drives)
		} mddev;
		struct { // Device Manager
			unsigned long disks;	// disks in DM
			char *level;		// DM level
			mdslave *slaves;	// DM components
			char *uuid;
			char *dmname;
			transport_e transport;
			char *pttable;		// Partition table type (can be NULL)
		} dmdev;
		struct { // Partitions are kept in on-disk order
			// The *partition* UUID, not the filesystem's or disk's
			char *uuid;
			// FIXME store this as multibyte char *
			wchar_t *pname;		// Partition name, if it has
						//  one (GPT has a UTF-16 name)
			unsigned pnumber;	// Partition number
			// The BIOS+MBR partition record (including the first
			// byte, the 'boot flag') and GPT attributes.
			unsigned long long flags;
			struct {
				unsigned extended: 1;	// MBR extended?
				unsigned logical: 1;	// MBR logical?
			} ptstate;
			unsigned ptype;		// see ptypes.h
			struct device *parent;
			uint64_t fsector,lsector;	// Inclusive, logical
			uintmax_t alignment;	/* alignment of partition
						 (largest 2^n dividing size) */
		} partdev;
		struct {
			transport_e transport;
			uint64_t zpoolver;	// zpool version
			unsigned long disks;	// vdevs in zpool
			char *level;		// zraid level
			unsigned state;		// POOL_STATE_[UN]AVAILABLE
		} zpool;
	};
	enum {
		LAYOUT_NONE,
		LAYOUT_MDADM,
		LAYOUT_DM,
		LAYOUT_PARTITION,
		LAYOUT_ZPOOL,
	} layout;
	struct device *parts;	// Partitions (can be NULL)
	dev_t devno;		// Don't expose this non-persistent datum
	statpack stats;		// Stats since device came online, as returned
				//  in most recent call to read_diskstats()
	statpack statdelta;	// Delta between the current value of stats and
				//  its previous value (after two samples)
	struct timeval statq;	// Timespan of statdelta. statdelta is
				//  defined iff statq is not all 0s.
	void *uistate;		// UI-managed opaque state
} device;

// A block device controller.
typedef struct controller {
	// FIXME if libpci doesn't know about the device, we still ought use
	// a name determined via inspection of sysfs, just as we do for disks
	char *name;		// From libpci database
	char *sysfs;		// Sysfs node
	char *driver;		// From sysfs, 'device/module'
	char *ident;		// Manufactured identifier to reference adapter
	char *fwver;		// Firmware version, if known
	char *biosver;		// BIOS version, if known
	enum {
		BUS_UNKNOWN,
		BUS_VIRTUAL,
		BUS_PCIe,
	} bus;
	enum {
		TRANSPORT_ATA,
		TRANSPORT_USB,
		TRANSPORT_USB2,
		TRANSPORT_USB3,
		TRANSPORT_NVME,
	} transport;
	int numa_node;		// -1: no NUMA in use
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
	uintmax_t bandwidth;	// Bandwidth in bits per second. 0 -> unknown.
	uintmax_t demand;	// Theoretical bandwidth in bits per second
				//  used by attached devices
	device *blockdevs;
	struct controller *next;
	dev_t devno;		// Don't expose this non-persistent datum
	void *uistate;		// UI-managed opaque state
} controller;

static inline uintmax_t
first_usable_sector(const device *d){
	if(d->layout == LAYOUT_NONE){
		return d->blkdev.first_usable;
	}
	return d->physsec / d->logsec;
}

static inline uintmax_t
last_usable_sector(const device *d){
	if(d->layout == LAYOUT_NONE){
		return d->blkdev.last_usable;
	}
	return d->size / d->logsec - 1;
}

// Currently, we just blindly hand out references to our internal store. This
// simply will not fly in the long run -- FIXME
const controller *get_controllers(void);

// These are similarly no good FIXME
device *lookup_device(const char *name);
controller *lookup_controller(const char *name);

// Supported partition table types
typedef struct pttable_type {
	char *name;
	char *desc;
} pttable_type;

// Supported partition table, partition, and filesystem types
pttable_type *get_ptable_types(int *);
void free_ptable_types(pttable_type *,int);

pttable_type *get_fs_types(int *);

// Supported aggregate types
typedef struct aggregate_type {
	const char *name;
	const char *desc;
	unsigned mindisks;
	unsigned maxfaulted;
	int (*makeagg)(const char *,char * const *,int);
	const char *defname;
	// If tokenreq is set, we need request some other token prior to
	// creating the aggregate. Display this prompt and acquire it.
	wchar_t *tokenreq;
} aggregate_type;

const aggregate_type *get_aggregate_types(int *);

int make_partition_table(device *,const char *);

int reset_controller(controller *);
int rescan_controller(controller *);

int rescan_blockdev(const device *);
int rescan_blockdev_blkrrpart(const device *);

int benchmark_blockdev(const device *);

// Very coarse locking
void lock_growlight(void);
void unlock_growlight(void);

int rescan_device(const char *);

void add_new_virtual_blockdev(device *);

int prepare_bios_boot(device *);
int prepare_uefi_boot(device *);

// Must match in all four ways: UUID, label, device name, and bus path. Any
// partial match is cause to fail the search, since it represents ambiguity.
device *match_device(const device *);

static inline const char *
pcie_gen(unsigned gen){
	switch(gen){
		case 1: return "1.0";
		case 2: return "2.0";
		case 3: return "3.0";
		default: return "unknown";
	}
}

static inline int
parttype_aggregablep(unsigned pt){
	const ptype *pptr;

	for(pptr = ptypes ; pptr->name ; ++pptr){
		if(pt == pptr->code){
			return pptr->aggregable;
		}
	}
	diag("Partition type %u unknown\n",pt);
	return 0;
}

// FIXME do away with this. use ptypes.[hc] exclusively
static inline const char *
partrole_str(unsigned pt,uint64_t flags){
	return ((pt == PARTROLE_PRIMARY) && (flags & 0xffu) == 0x80) ? "Boot" :
		pt == PARTROLE_ESP ? "ESP" :
		pt == PARTROLE_PRIMARY ? "Lnx" : "Oth";
}

static inline const char *
transport_str(transport_e t){
	return t == SERIAL_USB3 ? "USB3" : t == SERIAL_USB2 ? "USB2" :
		t == SERIAL_USB ? "USB1" : t == SERIAL_ATAIII ? "SAT3" :\
		t == SERIAL_ATAII ? "SAT2" :
	 	t == SERIAL_ATAI ? "SAT1" : t == SERIAL_ATA8 ? "ATA8" :
	 	t == SERIAL_UNKNOWN ? "SATA" : t == PARALLEL_ATA ? "PATA" :
		t == DIRECT_NVME ? "NVMe" :
	 	t == AGGREGATE_MIXED ? "Mix" : "?";
}

static inline uintmax_t
transport_bw(transport_e t){
  // FIXME NVMe is dependent on PCIe version and number of links
	return t == DIRECT_NVME ? 32000000000 :
    t == SERIAL_USB3 ? 5000000000 :
		t == SERIAL_USB2 ? 480000000 :
		t == SERIAL_USB ? 12000000 :
		t == SERIAL_ATAIII ? 6000000000 :
		(t == SERIAL_ATAII || t == SERIAL_ATA8) ? 3000000000 :
		(t == SERIAL_ATAI || t == SERIAL_UNKNOWN) ? 1500000000 :
		t == PARALLEL_ATA ? 133000000 : 0;
}

#define PREFIXSTRLEN 7  // Does not include a '\0' (xxx.xxU)
#define BPREFIXSTRLEN 9  // Does not include a '\0' (xxx.xxUi), i == prefix
#define PREFIXFMT "%7s"
#define BPREFIXFMT "%9s"

// Takes an arbitrarily large number, and prints it into a fixed-size buffer by
// adding the necessary SI suffix. Usually, pass a |PREFIXSTRLEN+1|-sized
// buffer to generate up to PREFIXSTRLEN characters.
//
// val: value to print
// decimal: scaling. '1' if none has taken place.
// buf: buffer in which string will be generated
// bsize: size of buffer. ought be at least PREFIXSTRLEN
// omitdec: inhibit printing of all-0 decimal portions
// mult: base of suffix system (1000 or 1024)
// uprefix: character to print following suffix ('i' for kibibytes basically).
//   only printed if suffix is actually printed (input >= mult).
//
// For full safety, pass in a buffer that can hold the decimal representation
// of the largest uintmax_t plus three (one for the unit, one for the decimal
// separator, and one for the NUL byte).
static inline const char *
genprefix(uintmax_t val,unsigned decimal,char *buf,size_t bsize,
			int omitdec,unsigned mult,int uprefix){
	const char prefixes[] = "KMGTPEY";
	unsigned consumed = 0;
	uintmax_t dv;

	assert(0 != decimal);
	assert(0 != mult);
	dv = mult;
	while((val / decimal) >= dv && consumed < strlen(prefixes)){
		dv *= mult;
		if(UINTMAX_MAX / dv < mult){ // watch for overflow
			break;
		}
		++consumed;
	}
	if(dv != mult){
		dv /= mult;
		val /= decimal;
		// Remainder is val % dv; we want a percentage as scaled integer
		unsigned remain = (val % dv) * 100 / dv;
		if(remain || omitdec == 0){
			// FIXME we throw the % 100 on remain to avoid a
			// format-truncation warning. remain ought always be
			// less than 100, since integer division goes to 0.
			snprintf(buf, bsize,"%ju.%02u%c%c",
					val / dv,
					remain % 100,
					prefixes[consumed - 1],
					uprefix);
		}else{
			snprintf(buf,bsize,"%ju%c%c",val / dv,prefixes[consumed - 1],uprefix);
		}
	}else{
		if(val % decimal || omitdec == 0){
			snprintf(buf,bsize,"%ju.%02ju",val / decimal,val % decimal);
		}else{
			snprintf(buf,bsize,"%ju",val / decimal);
		}
	}
	return buf;
}

// Mega, kilo, gigabytes
static inline const char *
qprefix(uintmax_t val,unsigned decimal,char *buf,size_t bsize,int omitdec){
	return genprefix(val,decimal,buf,bsize,omitdec,1000,'\0');
}

// Mibi, kebi, gibibytes
static inline const char *
bprefix(uintmax_t val,unsigned decimal,char *buf,size_t bsize,int omitdec){
	return genprefix(val,decimal,buf,bsize,omitdec,1024,'i');
}

static inline const char *
guidstr_be(const void *guid,char *str){
	const unsigned char *gc = guid;

	sprintf(str,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			gc[3],gc[2],gc[1],gc[0],gc[5],gc[4],gc[7],gc[6],gc[8],
			gc[9],gc[0xa],gc[0xb],gc[0xc],gc[0xd],gc[0xe],gc[0xf]);
	return str;
}

static inline const char *
guidstr(const void *guid,char *str){
	const unsigned char *gc = guid;

	sprintf(str,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			gc[0],gc[1],gc[2],gc[3],gc[4],gc[5],gc[6],gc[7],gc[8],
			gc[9],gc[0xa],gc[0xb],gc[0xc],gc[0xd],gc[0xe],gc[0xf]);
	return str;
}

// Uses the omphalos_ctx's ->diag function pointer. Acquires omphalos_ctx via
// lookup on a TSD (omphalos_ctx_key).
void diagnostic(const char *,...) __attribute__ ((format (printf,1,2)));

typedef struct logent {
        char *msg;
        time_t when;
} logent;

#define MAXIMUM_LOG_ENTRIES 1024

// Get up to the last n diagnostics. n should not be 0 nor greater than
// MAXIMUM_LOG_ENTRIES. If there are less than n present, they'll be copied
// into the first n logents; logent[n].msg will then be NULL.
int get_logs(unsigned,logent *);

static inline int
target_mode_p(void){
	return !!growlight_target;
}

static inline int
targeted_p(const device *d){
	unsigned z;

	if(!target_mode_p()){
		return 0;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(strncmp(d->mnt.list[z],growlight_target,strlen(growlight_target)) == 0){
			return 1;
		}
	}
	return 0;
}

static inline int
target_root_p(const device *d){
	unsigned z;

	if(!target_mode_p()){
		return 0;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(strcmp(d->mnt.list[z],growlight_target) == 0){
			return 1;
		}
	}
	return 0;
}

static inline int
string_included_p(const stringlist *sl,const char *s){
	unsigned z;

	for(z = 0 ; z < sl->count ; ++z){
		if(strcmp(sl->list[z],s) == 0){
			return 1;
		}
	}
	return 0;
}

static inline int
add_string(stringlist *sl,const char *s){
	char **tmp;

	if((tmp = realloc(sl->list,sizeof(*sl->list) * (sl->count + 1))) == NULL){
		return -1;
	}
	sl->list = tmp;
	if((sl->list[sl->count++] = strdup(s)) == NULL){
		return -1;
	}
	return 0;
}

static inline int
add_string_exclusive(stringlist *sl,const char *s){
	char **tmp;

	if(string_included_p(sl,s)){
		return 0;
	}
	if((tmp = realloc(sl->list,sizeof(*sl->list) * (sl->count + 1))) == NULL){
		return -1;
	}
	sl->list = tmp;
	if((sl->list[sl->count++] = strdup(s)) == NULL){
		return -1;
	}
	return 0;
}

static inline void
free_stringlist(stringlist *sl){
	unsigned z;

	for(z = 0 ; z < sl->count ; ++z){
		free(sl->list[z]);
	}
	free(sl->list);
	sl->list = NULL;
	sl->count = 0;
}

#ifdef HAVE_LIBZFS
#include <libzfs.h>
#else
#define POOL_STATE_ACTIVE 0
#endif

#ifdef __cplusplus
}
#endif

#endif

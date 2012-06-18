#ifndef GROWLIGHT_GROWLIGHT
#define GROWLIGHT_GROWLIGHT

#ifdef __cplusplus
extern "C" {
#endif

int verbf(const char *,...) __attribute__ ((format (printf,1,2)));

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#include <mounts.h>

#define FSLABELSIZ 17

	// This isn't really suitable for use as a library to programs beyond
	// growlight. Not yet, in any case.

int growlight_init(int,char * const *);
int growlight_stop(void);

struct device;

typedef struct mdslave {
	char *name;			// Name of component
	struct device *component;	// Block device holding component of
					//  mdadm device
	struct mdslave *next;		// Next in this md device
} mdslave;

// An (non-link) entry in the device hierarchy, representing a block device.
// A partition corresponds to one and only one block device (which of course
// might represent multiple devices, or maybe just a file mounted loopback).
typedef struct device {
	// next block device on this controller
	struct device *next;
	char name[NAME_MAX];		// Entry in /dev or /sys/block
	char *model,*revision,*sn;	// Arbitrary UTF-8 strings
	char *wwn;			// World Wide Name
	char *mnt;			// Active mount point
	char *mntops;			// Mount options
	// If the filesystem is not mounted, but is found, only mnttype will be
	// set from among mnt, mntops and mnttype
	char *mnttype;			// Type of mount
	mount *target;			// Future mount point
	uintmax_t size;			// Size in bytes
	unsigned logsec;		// Logical sector size
	unsigned physsec;		// Physical sector size
	// Ranges from 0 to 32565, 0 highest priority. For our purposes, we
	// also use -1, indicating "unused", and -2, indicating "not swap".
	enum {
		SWAP_INVALID = -2,
		SWAP_INACTIVE = -1,
		SWAP_MAXPRIO = 0,
		SWAP_MINPRIO = 65535,
	} swapprio;			// Priority as a swap device
	char *uuid;			// *Filesystem* UUID
	char *label;			// *Filesystem* label
	union {
		struct {
			enum {
				UNKNOWN_ATA,
				PARALLEL_ATA,
				SERIAL_UNKNOWN,
				SERIAL_ATA8,
				SERIAL_ATAI,
				SERIAL_ATAII,
				SERIAL_ATAIII,
			} transport;
			unsigned realdev: 1;	// Is itself a real block device
			unsigned removable: 1;	// Removable media
			unsigned rotate: 1;	// Rotational media / spinning platters
			unsigned wcache: 1;	// Write cache enabled
			unsigned biosboot: 1;	// Non-zero bytes in MBR code area
			void *biossha1;		// SHA1 of first 440 bytes
			char *pttable;		// Partition table type (can be NULL)
			char *serial;		// Serial number (can be NULL)
		} blkdev;
		struct {
			unsigned long disks;	// RAID disks in md
			char *level;		// RAID level
			mdslave *slaves;	// RAID components
			char *uuid;
			char *mdname;
		} mddev;
		struct {
			// These are the *partition* UUID and label, not the
			// filesystem's or disk's.
			char *uuid,*label;
			char *pname;		// Partition name, if it has
						//  one (GPT has a UTF-16 name).
			unsigned pnumber;	// Partition number
			// The BIOS+MBR partition record (including the first
			// byte, the 'boot flag') and GPT attributes.
			unsigned long long flags;
			enum {
				PARTROLE_UNKNOWN,
				PARTROLE_PRIMARY,	// BIOS+MBR
				PARTROLE_EXTENDED,
				PARTROLE_LOGICAL,
				PARTROLE_EPS,		// UEFI
				PARTROLE_GPT,
				PARTROLE_MAC,
				PARTROLE_PC98,
			} partrole;
		} partdev;
	};
	enum {
		LAYOUT_NONE,
		LAYOUT_MDADM,
		LAYOUT_PARTITION,
		LAYOUT_ZPOOL,
	} layout;
	struct device *parts;	// Partitions (can be NULL)
	dev_t devno;		// Don't expose this non-persistent datum
} device;

// A block device controller.
typedef struct controller {
	// FIXME if libpci doesn't know about the device, we still ought use
	// a name determined via inspection of sysfs, just as we do for disks
	char *name;		// From libpci database
	enum {
		BUS_UNKNOWN,
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
	device *blockdevs;
	struct controller *next;
	dev_t devno;		// Don't expose this non-persistent datum
} controller;

// Currently, we just blindly hand out references to our internal store. This
// simply will not fly in the long run -- FIXME
const controller *get_controllers(void);

// This is similarly no good FIXME
device *lookup_device(const char *name);

device *lookup_dentry(device *,const char *);

// Supported partition table types
const char **get_ptable_types(void);

// Supported filesystem types
const char **get_fs_types(void);

int make_partition_table(device *,const char *);

int reset_blockdev(device *);
void free_device(device *);

#ifdef __cplusplus
}
#endif

#endif

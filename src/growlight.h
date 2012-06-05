#ifndef GROWLIGHT_GROWLIGHT
#define GROWLIGHT_GROWLIGHT

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

	// This isn't really suitable for use as a library to programs beyond
	// growlight. Not yet, in any case.

int growlight_init(int,char * const *);
int growlight_stop(void);

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

// Currently, we just blindly hand out references to our internal store. This
// simply will not fly in the long run -- FIXME
const device *get_block_devices(void);
const controller *get_controllers(void);

#ifdef __cplusplus
}
#endif

#endif

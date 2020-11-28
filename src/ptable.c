// copyright 2012–2020 nick black
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/blkpg.h>

#include "mbr.h"
#include "apm.h"
#include "gpt.h"
#include "wchar.h"
#include "popen.h"
#include "msdos.h"
#include "ptypes.h"
#include "ptable.h"
#include "growlight.h"

#define LBA_SIZE 512
#define MBR_SIZE (LBA_SIZE - MBR_OFFSET)

static inline const char *
get_ptype(const device *d){
	if(d->layout == LAYOUT_NONE){
		if(d->blkdev.pttable == NULL){
			diag("No partition table on %s\n", d->name);
			return NULL;
		}
		return d->blkdev.pttable;
	}else if(d->layout == LAYOUT_MDADM){
		if(d->mddev.pttable == NULL){
			diag("No partition table on %s\n", d->name);
			return NULL;
		}
		return d->mddev.pttable;
	}else if(d->layout == LAYOUT_DM){
		if(d->dmdev.pttable == NULL){
			diag("No partition table on %s\n", d->name);
			return NULL;
		}
		return d->dmdev.pttable;
	}
	diag("Not a partitionable device: %s\n", d->name);
	return NULL;
}

static const struct ptable {
	const char *name;
	const char *desc;
	int (*make)(device *);				// Make partition table
	int (*zap)(device *);				// Zap partition table
	int (*add)(device *, const wchar_t *, uintmax_t, uintmax_t, unsigned long long);
	int (*del)(const device *);			// Delete partition
	int (*pname)(device *, const wchar_t *);		// Set partition name
	int (*uuid)(device *, const void *);		// Set partition UUID
	int (*flags)(device *, uint64_t);		// Reset partition flags
	int (*flag)(device *, uint64_t, unsigned);	// Set a partition flag
	int (*code)(device *, unsigned long long);	// Set partition code
	uintmax_t (*first)(const device *);	// Get first usable sector
	uintmax_t (*last)(const device *);	// Get last usable sector
} ptables[] = {
	{
		.name = "gpt",
		.desc = "GUID Partition Table",
		.make = new_gpt,
		.zap = zap_gpt,
		.add = add_gpt,
		.del = del_gpt,
		.pname = name_gpt,
		.uuid = uuid_gpt,
		.flags = flags_gpt,
		.flag = flag_gpt,
		.code = code_gpt,
		.first = first_gpt,
		.last = last_gpt,
	}, {
		.name = "dos",
		.desc = "IBMPC (DOS) / Master Boot Record",
		.make = new_msdos,
		.zap = zap_msdos,
		.add = add_msdos,
		.del = del_msdos,
		.pname = NULL,
		.uuid = NULL,
		.flags = flags_msdos,
		.flag = flag_msdos,
		.code = code_msdos,
		.first = first_msdos,
		.last = last_msdos,
	}, {
		.name = "apm",
		.desc = "Apple Partition Map",
		.make = new_apm,
		.zap = zap_apm,
		.add = NULL,
		.del = NULL,
		.pname = NULL,
		.uuid = NULL,
		.flag = NULL,
		.code = NULL,
		.first = first_apm,
		.last = last_apm,
	}, {
		.name = "mdp",
		.desc = "Linux MD partitioning",
		.make = NULL,
		.zap = NULL,
		.add = NULL,
		.del = NULL,
		.pname = NULL,
		.uuid = NULL,
		.flag = NULL,
		.code = NULL,
		.first = NULL,
		.last = NULL,
	}, {
		.name = NULL,
	}
};

// Only returns those ptable types we can create
pttable_type *get_ptable_types(int *count){
	pttable_type *pt;
	int z, w;

	*count = (sizeof(ptables) / sizeof(*ptables)) - 1;
	if(*count <= 0){
		diag("Invalid table type count (%d), aborting", *count);
		return NULL;
	}
	if((pt = malloc(sizeof(*pt) * *count)) == NULL){
		*count = 0;
		return NULL;
	}
	for(w = 0, z = 0 ; z < *count ; ++z){
		if(ptables[z].make == NULL){
			continue;
		}
		if((pt[w].name = strdup(ptables[z].name)) == NULL){
			goto err;
		}
		if((pt[w].desc = strdup(ptables[z].desc)) == NULL){
			free(pt[w].name);
			goto err;
		}
		++w;
	}
	*count = w;
	return pt;

err:
	while(w--){
		free(pt[w].name);
		free(pt[w].desc);
	}
	free(pt);
	*count = 0;
	return NULL;
}

void free_ptable_types(pttable_type *pt, int count){
	while(count--){
		free(pt[count].name);
		free(pt[count].desc);
	}
	free(pt);
}

int make_partition_table(device *d, const char *pty){
	const struct ptable *pt;

	if(d->layout != LAYOUT_NONE){
		diag("Will only create a partition table on raw block devices\n");
		return -1;
	}
	if(d->blkdev.pttable){
		diag("Partition table already exists on %s\n", d->name);
		return -1;
	}
	if(d->mnttype){
		diag("Filesystem exists on %s\n", d->name);
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, pty) == 0){
			if(pt->make(d)){
				return -1;
			}
			if(rescan_blockdev_blkrrpart(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", pty);
	return -1;
}

// Wipe the partition table (make it unrecognizable, preferably by overwriting
// it with zeroes). If a ptype is specified, it is assumed that this partition
// table type is being used, and we will zero out according to the specified
// type, even if it doesn't match the detected type (very dangerous!). If no
// type is specified, the detected type, if it exists, is used. If no type is
// specified and none is detected, nothing is done.
int wipe_ptable(device *d, const char *pty){
	const struct ptable *ptp;
	const device *p;
	const char *pt;

	if(d->layout != LAYOUT_NONE){
		diag("Will only remove partition tables from raw block devices\n");
		return -1;
	}
	if(d->mnt.count){
		diag("%s is mounted on %s; not removing\n", d->mnt.list[0], d->name);
		return -1;
	}
	for(p = d->parts ; p ; p = p->next){
		if(p->mnt.count){
			diag("%s is mounted on %s; not removing\n", p->mnt.list[0], p->name);
			return -1;
		}
	}
	if( !(pt = d->blkdev.pttable) ){
		if( (pt = pty) ){
			diag("No partition table on %s; wiping anyway\n", d->name);
		}else{
			diag("No partition table detected on %s\n", d->name);
			return -1;
		}
	}else if(pty && strcmp(pty, pt)){
		diag("Wiping %s table despite %s detection on %s\n", pty, pt, d->name);
		pt = pty;
	}
	for(ptp = ptables ; ptp->name ; ++ptp){
		if(strcmp(ptp->name, pt) == 0){
			if(ptp->zap(d)){
				return -1;
			}
			if(rescan_blockdev_blkrrpart(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", d->blkdev.pttable);
	return -1;
}

int add_partition(device *d, const wchar_t *name, uintmax_t fsec, uintmax_t lsec, unsigned long long code){
	const struct ptable *pt;
	const char *pty;

	if(d == NULL){
		diag("Passed NULL device\n");
		return -1;
	}
	if(lsec < fsec || lsec > last_usable_sector(d) || fsec < first_usable_sector(d)){
		diag("Bad sector spec (%ju:%ju) on %s\n", fsec, lsec, d->name);
		return -1;
	}
	if((pty = get_ptype(d)) == NULL){
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, pty) == 0){
			if(pt->add(d, name, fsec, lsec, code)){
				return -1;
			}
			if(rescan_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", pty);
	return -1;
}

int wipe_partition(const device *d){
	const char *pty = d->partdev.parent->blkdev.pttable;
	const struct ptable *pt;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only remove real partitions\n");
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, pty) == 0){
			if(pt->del == NULL){
				diag("Partition deletion not supported on %s\n", pty);
				return -1;
			}
			if(pt->del(d)){
				return -1;
			}
			if(rescan_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", d->blkdev.pttable);
	return -1;
}

int partitions_named_p(const device *d){
	const struct ptable *pt;

	if(!d){
		diag("Passed a NULL device\n");
		return -1;
	}
	if(d->layout != LAYOUT_NONE){
		diag("%s is not a block device\n", d->name);
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, d->blkdev.pttable) == 0){
			return !!pt->pname;
		}
	}
	diag("Unknown partition table type: %s\n", d->blkdev.pttable);
	return -1;
}

int name_partition(device *d, const wchar_t *name){
	const struct ptable *pt;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only name real partitions\n");
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, d->partdev.parent->blkdev.pttable) == 0){
			wchar_t *dp;

			if(!pt->pname){
				diag("Partition naming not supported on %s\n", d->partdev.parent->blkdev.pttable);
				return -1;
			}
			if((dp = wcsdup(name)) == NULL){
				diag("Bad name: %ls\n", name);
				return -1;
			}
			if(pt->pname(d, name)){
				free(dp);
				return -1;
			}
			free(d->partdev.pname);
			d->partdev.pname = dp;
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", d->blkdev.pttable);
	return -1;
}

int uuid_partition(device *d, const void *uuid){
	const struct ptable *pt;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only set UUID of real partitions\n");
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, d->partdev.parent->blkdev.pttable) == 0){
			if(!pt->uuid){
				diag("Partition UUIDs not supported on %s\n", d->partdev.parent->blkdev.pttable);
				return -1;
			}
			if(pt->uuid(d, uuid)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", d->blkdev.pttable);
	return -1;
}

int check_partition(device *d){
	if(d->mnt.count){
		diag("Will not check mounted filesystem %s\n", d->name);
		return -1;
	}
	if(d->mnttype == NULL){
		diag("No filesystem on %s\n", d->name);
		return -1;
	}
	// FIXME not every filesystem supports -y
	if(vspopen_drain("fsck.%s -y -C 0 /dev/%s", d->mnttype, d->name)){
		return -1;
	}
	return 0;
}

int partition_set_flags(device *d, uint64_t flag){
	const struct ptable *pt;
	const char *pttable;

	assert(d);
	if(d->layout != LAYOUT_PARTITION){
		diag("Will only set flags on real partitions\n");
		return -1;
	}
	if((pttable = get_ptype(d->partdev.parent)) == NULL){
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, pttable) == 0){
			if(!pt->flags){
				diag("Partition flags not supported on %s\n", d->partdev.parent->blkdev.pttable);
				return -1;
			}
			if(pt->flags(d, flag)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", pttable);
	return -1;
}

int partition_set_flag(device *d, uint64_t flag, unsigned state){
	const struct ptable *pt;
	const char *pttable;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only set flags on real partitions\n");
		return -1;
	}
	if((pttable = get_ptype(d->partdev.parent)) == NULL){
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, pttable) == 0){
			if(!pt->flag){
				diag("Partition flags not supported on %s\n", d->partdev.parent->blkdev.pttable);
				return -1;
			}
			if(pt->flag(d, flag, state)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", pttable);
	return -1;
}

int partition_set_code(device *d, unsigned long long code){
	const struct ptable *pt;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only set type of real partitions\n");
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, d->partdev.parent->blkdev.pttable) == 0){
			if(!pt->code){
				diag("Partition code not supported on %s\n", d->partdev.parent->blkdev.pttable);
				return -1;
			}
			if(pt->code(d, code)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n", d->blkdev.pttable);
	return -1;
}

// both of these functions smell, and might be broken for partition tables with
// empty space at the beginning or ending see #61 FIXME
uintmax_t lookup_first_usable_sector(const device *d){
	/*
	if(d->logsec == 0){
		return 0;
	}
  return d->physsec / d->logsec;
	*/
	const struct ptable *pt;
	if(d->layout != LAYOUT_NONE || d->blkdev.pttable == NULL){
		return d->physsec / d->logsec;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, d->blkdev.pttable) == 0){
			return pt->first(d);
		}
	}
	return 0;
}

uintmax_t lookup_last_usable_sector(const device *d){
	if(d->logsec == 0){
		return 0;
	}
  return d->size / d->logsec - 1;
	/*if(d->layout != LAYOUT_NONE || d->blkdev.pttable == NULL){
		return d->size / d->logsec - 1;
	}
  if(strcmp(d->blkdev.pttable, "gpt") == 0){
		return d->size / d->logsec - 2;
  }
	return d->size / d->logsec - 1;
	const struct ptable *pt;
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name, d->blkdev.pttable) == 0){
			return pt->last(d);
		}
	}*/
	return 0;
}

// Uses the BLKPG ioctl to notify the kernel that a partition has been added
int blkpg_add_partition(int fd, long long start, long long len, int pno, const char *name){
	struct blkpg_partition data;
	struct blkpg_ioctl_arg blk;
	unsigned t;

	if(pno < 1){
		diag("Invalid partition number: %d\n", pno);
		return -1;
	}
	if(strlen(name) >= sizeof(data.devname)){
		diag("Name too long for BLKPG: %s\n", name);
		return -1;
	}
	memset(&blk, 0, sizeof(blk));
	memset(&data, 0, sizeof(data));
	blk.op = BLKPG_ADD_PARTITION;
	blk.datalen = sizeof(data);
	blk.data = &data;
	data.start = start;
	data.length = len;
	data.pno = pno;
	strcpy(data.devname, name);
	for(t = 0 ; t < 2 ; ++t){
		if(ioctl(fd, BLKPG, &blk) == 0){
			goto success;
		}
		diag("Error invoking BLKPG ioctl on %d p%d (%s?), retrying in 3s\n", fd, pno, strerror(errno));
		sleep(3);
	}
	if(ioctl(fd, BLKPG, &blk) == 0){
		goto success;
	}
	diag("Error invoking BLKPG ioctl on %d p%d (%s?)\n", fd, pno, strerror(errno));
	return -1;

success:
	diag("Informed kernel of partition %d's creation\n", pno);
	return 0;
}

// Uses the BLKPG ioctl to notify the kernel that a partition has been removed
int blkpg_del_partition(int fd, long long start, long long len, int pno, const char *name){
	struct blkpg_partition data;
	struct blkpg_ioctl_arg blk;
	unsigned t;

	if(pno < 1){
		diag("Invalid partition number: %d\n", pno);
		return -1;
	}
	if(strlen(name) >= sizeof(data.devname)){
		diag("Name too long for BLKPG: %s\n", name);
		return -1;
	}
	memset(&blk, 0, sizeof(blk));
	memset(&data, 0, sizeof(data));
	blk.op = BLKPG_DEL_PARTITION;
	blk.datalen = sizeof(data);
	blk.data = &data;
	data.start = start;
	data.length = len;
	data.pno = pno;
	strcpy(data.devname, name);
	for(t = 0 ; t < 2 ; ++t){
		if(ioctl(fd, BLKPG, &blk) == 0){
			goto success;
		}
		diag("Error invoking BLKPG ioctl on %d (%s?), retrying in 3s\n", pno, strerror(errno));
		sleep(3);
	}
	if(ioctl(fd, BLKPG, &blk) == 0){
		goto success;
	}
	diag("Error invoking BLKPG ioctl on %d (%s?)\n", pno, strerror(errno));
	return -1;

success:
	diag("Informed kernel of partition %d's deletion\n", pno);
	return 0;
}

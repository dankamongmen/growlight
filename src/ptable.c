#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbr.h"
#include "gpt.h"
#include "wchar.h"
#include "popen.h"
#include "ptable.h"
#include "growlight.h"

static int
dos_make_table(device *d){
	if(vspopen_drain("parted /dev/%s mklabel msdos",d->name)){
		return -1;
	}
	return 0;
}

static int
dos_zap_table(device *d){
	return wipe_dos_ptable(d);
}

static int
dos_add_part(device *d,const wchar_t *name,uintmax_t size){
	if(name){
		diag("Names are not supported for MBR partitions!\n");
		return -1;
	}
	if(size > 2ull * 1000ull * 1000ull * 1000ull * 1000ull){
		diag("MBR partitions may not exceed 2TB\n");
		return -1;
	}
	// FIXME
	diag("FIXME: I don't like dos partitions! %s\n",d->name);
	return -1;
}

static const struct ptable {
	const char *name;
	int (*make)(device *);
	int (*zap)(device *);
	int (*add)(device *,const wchar_t *,uintmax_t);
} ptables[] = {
	{
		.name = "gpt",
		.make = new_gpt,
		.zap = zap_gpt,
		.add = add_gpt,
	},
	{
		.name = "dos",
		.make = dos_make_table,
		.zap = dos_zap_table,
		.add = dos_add_part,
	},
	{ .name = NULL, }
};

// FIXME need initialize values
static const char *ptable_types[sizeof(ptables) / sizeof(*ptables) + 1];

const char **get_ptable_types(void){
	static unsigned once;

	// FIXME thread-unsafe
	if(!once){
		unsigned z;

		for(z = 0 ; z < sizeof(ptables) / sizeof(*ptables) ; ++z){
			ptable_types[z] = ptables[z].name;
		}
		once = 1;
	}
	return ptable_types;
}

int make_partition_table(device *d,const char *ptype){
	const struct ptable *pt;

	if(d->layout != LAYOUT_NONE){
		diag("Will only create a partition table on raw block devices\n");
		return -1;
	}
	if(d->blkdev.pttable){
		diag("Partition table already exists on %s\n",d->name);
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name,ptype) == 0){
			if(pt->make(d)){
				return -1;
			}
			if(rescan_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n",ptype);
	return -1;
}

// Wipe the partition table (make it unrecognizable, preferably by overwriting
// it with zeroes). If a ptype is specified, it is assumed that this partition
// table type is being used, and we will zero out according to the specified
// type, even if it doesn't match the detected type (very dangerous!). If no
// type is specified, the detected type, if it exists, is used.
int wipe_ptable(device *d,const char *ptype){
	const struct ptable *ptp;
	const char *pt;

	if(d->layout != LAYOUT_NONE){
		diag("Will only remove partition tables from raw block devices\n");
		return -1;
	}
	if( !(pt = d->blkdev.pttable) ){
		if( (pt = ptype) ){
			diag("No partition table on %s; wiping anyway\n",d->name);
		}else{
			diag("No partition table detected on %s\n",d->name);
			return -1;
		}
	}else if(ptype && strcmp(pt,ptype)){
		diag("Wiping %s table despite %s detection on %s\n",ptype,pt,d->name);
	}
	for(ptp = ptables ; ptp->name ; ++ptp){
		if(strcmp(ptp->name,pt) == 0){
			if(ptp->zap(d)){
				return -1;
			}
			if(rescan_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n",d->blkdev.pttable);
	return -1;
}

int add_partition(device *d,const wchar_t *name,size_t size){
	const struct ptable *pt;

	if(d->layout != LAYOUT_NONE){
		diag("Will only add partitions to real block devices\n");
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name,d->blkdev.pttable) == 0){
			if(pt->add(d,name,size)){
				return -1;
			}
			if(rescan_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n",d->blkdev.pttable);
	return -1;
}

int wipe_partition(device *d){
	char cmd[PATH_MAX];
	device *p;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only remove actual partitions\n");
		return -1;
	}
	p = d->partdev.parent;
	if(snprintf(cmd,sizeof(cmd),"gdisk /dev/%s --delete=%u",p->name,d->partdev.pnumber) >= (int)sizeof(cmd)){
		diag("Bad names: %s / %s\n",p->name,d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	if(rescan_blockdev(p)){
		return -1;
	}
	return 0;
}

int name_partition(device *d,const wchar_t *name){
	char cmd[BUFSIZ];
	wchar_t *dup;
	device *par;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only name actual partitions\n");
		return -1;
	}
	par = d->partdev.parent;
	if(d->partdev.partrole != PARTROLE_GPT && d->partdev.partrole != PARTROLE_EPS
			/*&& d->partdev.partrole != PARTROLE_PC98
			&& d->partdev.partrole != PARTROLE_MAC*/){
		diag("Cannot name %s; bad partition table type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"sgdisk /dev/%s --change-name=%u:%ls",par->name,d->partdev.pnumber,name) >= (int)sizeof(cmd)){
		diag("Bad names: %s / %u / %ls\n",par->name,d->partdev.pnumber,name);
		return -1;
	}
	if((dup = wcsdup(name)) == NULL){
		diag("Bad name: %ls\n",name);
		return -1;
	}
	if(popen_drain(cmd)){
		free(dup);
		return -1;
	}
	free(d->partdev.pname);
	d->partdev.pname = dup;
	return 0;
}

int check_partition(device *d){
	char cmd[PATH_MAX];

	if(d->mnt){
		diag("Will not check mounted filesystem %s\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"fsck -C 0 /dev/%s",d->name) >= (int)sizeof(cmd)){
		diag("Bad name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

int partition_set_flag(device *d,uint64_t flag,unsigned state){
	char cmd[BUFSIZ];
	device *par;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only set flags on actual partitions\n");
		return -1;
	}
	par = d->partdev.parent;
	if(d->partdev.partrole == PARTROLE_PRIMARY){
		if(flag != 0x80){
			diag("Invalid flag for BIOS/MBR: 0x%016jx\n",(uintmax_t)flag);
			return -1;
		}
		// FIXME set it!
	}else if(d->partdev.partrole != PARTROLE_GPT){
		diag("Cannot set flags on %s; bad partition type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"sgdisk -A %u:%s:%jx /dev/%s",
				d->partdev.pnumber,state ? "set" : "clear",
				(uintmax_t)flag,par->name) >= (int)sizeof(cmd)){
		diag("Bad name: %s\n",par->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

int partition_set_code(device *d,unsigned code){
	char cmd[BUFSIZ];
	device *par;

	if(d->layout != LAYOUT_PARTITION){
		diag("Will only set code on actual partitions\n");
		return -1;
	}
	par = d->partdev.parent;
	if(d->partdev.partrole == PARTROLE_PRIMARY){
		if(code > 0xff){
			diag("Invalid type for BIOS/MBR: %08u\n",code);
			return -1;
		}
		// FIXME set it!
	}else if(d->partdev.partrole != PARTROLE_GPT){
		diag("Cannot set code on %s; bad partition type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"sgdisk -t %u:%04x /dev/%s",
			d->partdev.pnumber,code,par->name) >= (int)sizeof(cmd)){
		diag("Bad name: %s\n",par->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbr.h"
#include "wchar.h"
#include "popen.h"
#include "ptable.h"
#include "growlight.h"

static int
gpt_make_table(device *d){
	char cmd[BUFSIZ];

	if(snprintf(cmd,sizeof(cmd),"/sbin/parted /dev/%s mklabel gpt",d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

static int
gpt_zap_table(device *d){
	char cmd[BUFSIZ];

	if(snprintf(cmd,sizeof(cmd),"/sbin/sgdisk --zap-all /dev/%s",d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

static int
dos_make_table(device *d){
	char cmd[BUFSIZ];

	if(snprintf(cmd,sizeof(cmd),"/sbin/parted /dev/%s mklabel msdos",d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

static int
dos_zap_table(device *d){
	return wipe_dos_ptable(d);
}

static int
gpt_add_part(device *d,const wchar_t *name,uintmax_t size){
	uintmax_t sectors;
	char cmd[BUFSIZ];
	unsigned partno;
	const device *p;

	if(!name){
		fprintf(stderr,"GPT partitions ought be named!\n");
		return -1;
	}
	// FIXME sgdisk uses the old 512 value, not the appropriate-for-device size
	sectors = size / 512;
	partno = 1;
	for(p = d->parts ; p ; p = p->next){
		if(partno == p->partdev.pnumber){
			const device *pcheck;

			do{
				++partno;
				for(pcheck = d->parts ; pcheck != p ; pcheck = pcheck->next){
					if(partno == pcheck->partdev.pnumber){
						break;
					}
				}
			}while(p != pcheck);
		}
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/sgdisk --new=%u:0:%ju /dev/%s",partno,sectors,d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/sgdisk --change-name=%u:%ls /dev/%s",partno,name,d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad names: %d / %ls\n",d->partdev.pnumber,name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

static int
dos_add_part(device *d,const wchar_t *name,uintmax_t size){
	if(name){
		fprintf(stderr,"Names are not supported for MBR partitions!\n");
		return -1;
	}
	if(size > 2ull * 1000ull * 1000ull * 1000ull * 1000ull){
		fprintf(stderr,"MBR partitions may not exceed 2TB\n");
		return -1;
	}
	// FIXME
	fprintf(stderr,"FIXME: I don't like dos partitions! %s\n",d->name);
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
		.make = gpt_make_table,
		.zap = gpt_zap_table,
		.add = gpt_add_part,
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
		fprintf(stderr,"Will only create a partition table on raw block devices\n");
		return -1;
	}
	if(d->blkdev.pttable){
		fprintf(stderr,"Partition table already exists on %s\n",d->name);
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
	fprintf(stderr,"Unsupported partition table type: %s\n",ptype);
	return -1;
}

int wipe_ptable(device *d){
	const struct ptable *pt;

	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Will only remove partition tables from raw block devices\n");
		return -1;
	}
	if(!d->blkdev.pttable){
		fprintf(stderr,"No partition table exists on %s\n",d->name);
		return -1;
	}
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name,d->blkdev.pttable) == 0){
			if(pt->zap(d)){
				return -1;
			}
			if(rescan_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	fprintf(stderr,"Unsupported partition table type: %s\n",d->blkdev.pttable);
	return -1;
}

int add_partition(device *d,const wchar_t *name,size_t size){
	const struct ptable *pt;

	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Will only add partitions to real block devices\n");
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
	fprintf(stderr,"Unsupported partition table type: %s\n",d->blkdev.pttable);
	return -1;
}

int wipe_partition(device *d){
	char cmd[PATH_MAX];
	device *p;

	if(d->layout != LAYOUT_PARTITION){
		fprintf(stderr,"Will only remove actual partitions\n");
		return -1;
	}
	p = d->partdev.parent;
	if(snprintf(cmd,sizeof(cmd),"/sbin/gdisk /dev/%s --delete=%u",p->name,d->partdev.pnumber) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad names: %s / %s\n",p->name,d->name);
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
		fprintf(stderr,"Will only name actual partitions\n");
		return -1;
	}
	par = d->partdev.parent;
	if(d->partdev.partrole != PARTROLE_GPT && d->partdev.partrole != PARTROLE_EPS
			/*&& d->partdev.partrole != PARTROLE_PC98
			&& d->partdev.partrole != PARTROLE_MAC*/){
		fprintf(stderr,"Cannot name %s; bad partition table type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/sgdisk /dev/%s --change-name=%u:%ls",par->name,d->partdev.pnumber,name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad names: %s / %u / %ls\n",par->name,d->partdev.pnumber,name);
		return -1;
	}
	if((dup = wcsdup(name)) == NULL){
		fprintf(stderr,"Bad name: %ls\n",name);
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
		fprintf(stderr,"Will not check mounted filesystem %s\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/fsck -C 0 /dev/%s",d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",d->name);
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
		fprintf(stderr,"Will only set flags on actual partitions\n");
		return -1;
	}
	par = d->partdev.parent;
	if(d->partdev.partrole == PARTROLE_PRIMARY){
		if(flag != 0x80){
			fprintf(stderr,"Invalid flag for BIOS/MBR: %016lu\n",flag);
			return -1;
		}
		// FIXME set it!
	}else if(d->partdev.partrole != PARTROLE_GPT){
		fprintf(stderr,"Cannot set flags on %s; bad partition type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/sgdisk -A %u:%s:%lx /dev/%s",
				d->partdev.pnumber,state ? "set" : "clear",
				flag,par->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",par->name);
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
		fprintf(stderr,"Will only set code on actual partitions\n");
		return -1;
	}
	par = d->partdev.parent;
	if(d->partdev.partrole == PARTROLE_PRIMARY){
		if(code > 0xff){
			fprintf(stderr,"Invalid type for BIOS/MBR: %08u\n",code);
			return -1;
		}
		// FIXME set it!
	}else if(d->partdev.partrole != PARTROLE_GPT){
		fprintf(stderr,"Cannot set code on %s; bad partition type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/sgdisk -t %u:%04x /dev/%s",
			d->partdev.pnumber,code,par->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad name: %s\n",par->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbr.h>
#include <popen.h>
#include <growlight.h>

static int
gpt_make_table(device *d){
	char cmd[PATH_MAX];

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
	char cmd[PATH_MAX];

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
	char cmd[PATH_MAX];

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

static const struct ptable {
	const char *name;
	int (*make)(device *);
	int (*zap)(device *);
} ptables[] = {
	{
		.name = "gpt",
		.make = gpt_make_table,
		.zap = gpt_zap_table,
	},
	{
		.name = "dos",
		.make = dos_make_table,
		.zap = dos_zap_table,
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
			if(reset_blockdev(d)){
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
			if(reset_blockdev(d)){
				return -1;
			}
			return 0;
		}
	}
	fprintf(stderr,"Unsupported partition table type: %s\n",d->blkdev.pttable);
	return -1;
}

int add_partition(device *d,const char *name,size_t size){
	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Will only add partitions to real block devices\n");
		return -1;
	}
	fprintf(stderr,"FIXME not yet implemented for %s / %zu\n",name,size);
	return -1;
}

int wipe_partition(device *p,device *d){
	char cmd[PATH_MAX];

	if(d->layout != LAYOUT_PARTITION){
		fprintf(stderr,"Will only remove actual partitions\n");
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/parted /dev/%s rm /dev/%s",p->name,d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad names: %s / %s\n",p->name,d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	if(reset_blockdev(p)){
		return -1;
	}
	return 0;
}

int name_partition(device *par,device *d,const char *name){
	char cmd[PATH_MAX];
	char *dup;

	if(d->layout != LAYOUT_PARTITION){
		fprintf(stderr,"Will only name actual partitions\n");
		return -1;
	}
	if(d->partdev.partrole != PARTROLE_GPT && d->partdev.partrole != PARTROLE_EPS
			&& d->partdev.partrole != PARTROLE_PC98
			&& d->partdev.partrole != PARTROLE_MAC){
		fprintf(stderr,"Cannot name %s; bad partition table type\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/parted /dev/%s name %u %s",par->name,d->partdev.pnumber,name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Bad names: %s / %u / %s\n",par->name,d->partdev.pnumber,name);
		return -1;
	}
	if((dup = strdup(name)) == NULL){
		fprintf(stderr,"Bad name: %s\n",name);
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

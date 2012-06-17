#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <mbr.h>
#include <popen.h>
#include <growlight.h>

static int
gpt_make_table(device *d){
	assert(d);
	return -1;
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
	// FIXME
	assert(d);
	return -1;
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

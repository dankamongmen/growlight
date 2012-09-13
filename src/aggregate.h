#ifndef GROWLIGHT_AGGREGATE
#define GROWLIGHT_AGGREGATE

#include "growlight.h"

static inline int
aggregate_default_p(const char *aggtype){
	return !strcmp(aggtype,"raidz2");
}

static inline int
mnttype_aggregablep(const char *mnttype){
	if(mnttype == NULL){
		return 1;
	}else if(strcmp(mnttype,"zfs_member") == 0){
		return 1;
	}else if(strcmp(mnttype,"linux_raid_member") == 0){
		return 1;
	}
	return 0;
}

static inline int
device_aggregablep(const device *d){
	if(!mnttype_aggregablep(d->mnttype)){
		return 0;
	}
	if(d->slave){
		return 0;
	}
	if(d->size == 0){
		return 0;
	}
	if(d->roflag){
		return 0;
	}
	switch(d->layout){
		case LAYOUT_NONE:
			if(d->blkdev.unloaded){
				return 0;
			}
			if(d->blkdev.pttable){
				return 0;
			}
			break;
		case LAYOUT_PARTITION:
			if(!parttype_aggregablep(d->partdev.ptype)){
				return 0;
			}
			break;
		case LAYOUT_MDADM:
		case LAYOUT_DM:
		case LAYOUT_ZPOOL:
			break;
	}
	return 1;
}

const aggregate_type *get_aggregate(const char *);

int assemble_aggregates(void);

#endif

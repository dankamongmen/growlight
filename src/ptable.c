#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <growlight.h>

static const struct ptable {
	const char *name;
} ptables[] = {
	{ .name = "gpt", },
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
	for(pt = ptables ; pt->name ; ++pt){
		if(strcmp(pt->name,ptype) == 0){
			// FIXME
			if(reset_blockdev(d)){
				return -1;
			}
			return -1;
		}
	}
	fprintf(stderr,"Unsupported partition table type: %s\n",ptype);
	return -1;
}

int wipe_ptable(device *d){
	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Will only create a partition table on raw block devices\n");
		return -1;
	}
	// FIXME
	return -1;
}

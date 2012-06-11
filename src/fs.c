#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <growlight.h>

static const struct fs {
	const char *name;
} fss[] = {
	{ .name = "ext4", },
	{ .name = NULL, }
};

// FIXME need initialize values
static const char *fs_types[sizeof(fss) / sizeof(*fss) + 1];

const char **get_fs_types(void){
	static unsigned once;

	// FIXME thread-unsafe
	if(!once){
		unsigned z;

		for(z = 0 ; z < sizeof(fss) / sizeof(*fss) ; ++z){
			fs_types[z] = fss[z].name;
		}
		once = 1;
	}
	return fs_types;
}

int make_filesystem(device *d,const char *ptype){
	const struct fs *pt;

	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Will only create a partition table on raw block devices\n");
		return -1;
	}
	for(pt = fss ; pt->name ; ++pt){
		if(strcmp(pt->name,ptype) == 0){
			// FIXME
			return 0;
		}
	}
	fprintf(stderr,"Unsupported partition table type: %s\n",ptype);
	return -1;
}

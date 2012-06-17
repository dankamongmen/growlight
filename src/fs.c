#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <growlight.h>

// FIXME surely there's a less grotesque way of doing this? parse blkid -k?
static const struct fs {
	const char *name;
} fss[] = {
	{ .name = "vfat", },
	{ .name = "swsuspend", },
	{ .name = "swap", },
	{ .name = "xfs", },
	{ .name = "ext4dev", },
	{ .name = "ext4", },
	{ .name = "ext3", },
	{ .name = "ext2", },
	{ .name = "jbd", },
	{ .name = "reiserfs", },
	{ .name = "reiser4", },
	{ .name = "jfs", },
	{ .name = "udf", },
	{ .name = "iso9660", },
	{ .name = "zfs_member", },
	{ .name = "hfsplus", },
	{ .name = "hfs", },
	{ .name = "ufs", },
	{ .name = "hpfs", },
	{ .name = "sysv", },
	{ .name = "xenix", },
	{ .name = "ntfs", },
	{ .name = "cramfs", },
	{ .name = "romfs", },
	{ .name = "minix", },
	{ .name = "gfs", },
	{ .name = "gfs2", },
	{ .name = "ocfs", },
	{ .name = "ocfs2", },
	{ .name = "oracleasm", },
	{ .name = "vxfs", },
	{ .name = "squashfs", },
	{ .name = "nss", },
	{ .name = "btrfs", },
	{ .name = "ubifs", },
	{ .name = "bfs", },
	{ .name = "VMFS", },
	{ .name = "befs", },
	{ .name = "nilfs2", },
	{ .name = "exfat", },
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

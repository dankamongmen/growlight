#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mmap.h"
#include "popen.h"
#include "growlight.h"

static int
ext4_mkfs(const char *dev){
	// if we're an mdadm, get chunk size and pass it as -Estride= FIXME
	// same for stripe_width FIXME
	// need -F for non-partition or block special FIXME
	// pass -M with mount point FIXME
	// take -L argument from user FIXME
	// allow a UUID to be supplied FIXME
	// set creator OS FIXME
	// allow -c (badblock check) FIXME
	if(vspopen_drain("mkfs -t ext4 -b -2048 -E lazy_itable_init=0,lazy_journal_init=0 -L SprezzaExt4 -O dir_index,extent,^uninit_bg %s",dev)){
		return -1;
	}
	return 0;
}

// FIXME surely there's a less grotesque way of doing this? parse blkid -k?
static const struct fs {
	const char *name;
	int (*mkfs)(const char *);
} fss[] = {
	{ .name = "vfat", },
	{ .name = "swsuspend", },
	{ .name = "swap", },
	{ .name = "xfs", },
	{ .name = "ext4dev", },
	{
		.name = "ext4",
		.mkfs = ext4_mkfs,
	},
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

	if(d->target){
		diag("Won't create fs on target mount %s (%s)\n",
				d->name,d->target->path);
		return -1;
	}
	if(d->mnt){
		diag("Won't create fs on active mount %s (%s)\n",
				d->name,d->mnt);
		return -1;
	}
	if(d->swapprio >= SWAP_MAXPRIO){
		diag("Won't create fs on active mount %s (%s)\n",
				d->name,d->mnt);
		return -1;
	}
	for(pt = fss ; pt->name ; ++pt){
		if(strcmp(pt->name,ptype) == 0){
			char dbuf[PATH_MAX],*mnttype;

			if(snprintf(dbuf,sizeof(dbuf),"/dev/%s",d->name) >= (int)sizeof(dbuf)){
				diag("Bad name: %s\n",d->name);
				return -1;
			}
			if(pt->mkfs == NULL){
				diag("Don't know how to make %s\n",ptype);
				return -1;
			}
			if((mnttype = strdup(ptype)) == NULL){
				return -1;
			}
			// FIXME needs accept/set UUID and label!
			if(pt->mkfs(dbuf)){
				free(mnttype);
				return -1;
			}
			// FIXME reprobe device?
			free(d->mnttype);
			d->mnttype = mnttype;
			return 0;
		}
	}
	diag("Unsupported partition table type: %s\n",ptype);
	return -1;
}

int parse_filesystems(const glightui *gui __attribute__ ((unused)),const char *fn){
	off_t len,idx;
	char *map;
	int fd;

	if((map = map_virt_file(fn,&fd,&len)) == MAP_FAILED){
		return -1;
	}
	idx = 0;
	while(idx < len){
		off_t fsstart;
		int virt = 0;

		while(idx < len && isspace(map[idx])){
			++idx;
		}
		if(len - idx >= (off_t)strlen("nodev")){
			if(strncmp(map + idx,"nodev",strlen("nodev")) == 0){
				idx += strlen("nodev");
				virt = 1;
			}
		}
		while(idx < len && isspace(map[idx])){
			++idx;
		}
		fsstart = idx;
		while(idx < len && !isspace(map[idx])){
			++idx;
		}
		if(fsstart < len){
			verbf("%sfilesystem support: %*.*s\n",
					virt ? "virtual " : "",
					(int)(idx - fsstart),
					(int)(idx - fsstart),map + fsstart);
		}
	}
	munmap_virt(map,len);
	close(fd);
	return 0;
}

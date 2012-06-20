#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "growlight.h"

static int
ext4_mkfs(const char *dev){
	char cmd[PATH_MAX],buf[BUFSIZ];
	ssize_t r;
	FILE *fp;

	// if we're an mdadm, get chunk size and pass it as -Estride= FIXME
	// same for stripe_width FIXME
	// need -F for non-partition or block special FIXME
	// pass -M with mount point FIXME
	// take -L argument from user FIXME
	// allow a UUID to be supplied FIXME
	// set creator OS FIXME
	// allow -c (badblock check) FIXME
	if(snprintf(cmd,sizeof(cmd),"/sbin/mkfs -t ext4 -b -2048 -E lazy_itable_init=0,lazy_journal_init=0 -L SprezzaExt4 -O dir_index,extent,^uninit_bg %s",dev) >= (int)sizeof(cmd)){
		fprintf(stderr,"Error building command line for %s\n",dev);
		return -1;
	}
	if((fp = popen(cmd,"r")) == NULL){
		fprintf(stderr,"Error running '%s' (%s?)\n",cmd,strerror(errno));
		return -1;
	}
	// FIXME probably best to do non-blocking reads?
	while((r = fread(buf,1,BUFSIZ,fp)) > 0){
		if(fwrite(buf,1,r,stdout) < (size_t)r){
			fprintf(stderr,"Error echoing '%s' (%s?)\n",cmd,strerror(errno));
			fclose(fp);
			return -1;
		}
	}
	if(r < 0){
		fprintf(stderr,"Error draining '%s' (%s?)\n",cmd,strerror(errno));
		fclose(fp);
		return -1;
	}
	if(fclose(fp)){
		fprintf(stderr,"Error running '%s'\n",cmd);
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
		fprintf(stderr,"Won't create fs on target mount %s (%s)\n",
				d->name,d->target->path);
		return -1;
	}
	if(d->mnt){
		fprintf(stderr,"Won't create fs on active mount %s (%s)\n",
				d->name,d->mnt);
		return -1;
	}
	if(d->swapprio >= SWAP_MAXPRIO){
		fprintf(stderr,"Won't create fs on active mount %s (%s)\n",
				d->name,d->mnt);
		return -1;
	}
	for(pt = fss ; pt->name ; ++pt){
		if(strcmp(pt->name,ptype) == 0){
			char dbuf[PATH_MAX],*mnttype;

			if(snprintf(dbuf,sizeof(dbuf),"/dev/%s",d->name) >= (int)sizeof(dbuf)){
				fprintf(stderr,"Bad name: %s\n",d->name);
				return -1;
			}
			if(pt->mkfs == NULL){
				fprintf(stderr,"Don't know how to make %s\n",ptype);
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
	fprintf(stderr,"Unsupported partition table type: %s\n",ptype);
	return -1;
}

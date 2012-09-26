#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/swap.h>

#include "zfs.h"
#include "mmap.h"
#include "popen.h"
#include "growlight.h"

static int
create_btrfs(const char *dev,const struct mkfsmarshal *mkm){
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaBTRFS";
	}
	if(vspopen_drain("mkfs.btrfs -h -L \"%s\" %s",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
hfs_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaHFS";
	}
	if(vspopen_drain("mkfs.hfs -h -v \"%s\" %s",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
hfsplus_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaHFS+";
	}
	if(vspopen_drain("mkfs.hfsplus -s -J -v \"%s\" %s",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
jfs_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// allow -c (badblock check) FIXME
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaJFS";
	}
	// FIXME what about external journals?
	if(vspopen_drain("mkfs.jfs -L \"%s\" %s",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
xfs_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// allow -c (badblock check) FIXME
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaXFS";
	}
	// FIXME set -s to the physical sector size
	if(vspopen_drain("mkfs.xfs %s-L \"%s\" %s",
			mkm->force ? "-f ": "",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
create_ntfs(const char *dev,const struct mkfsmarshal *mkm){
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaNTFS";
	}
	if(vspopen_drain("mkfs.ntfs -v %s-U -L \"%s\" %s",
			mkm->force ? "-F " : "",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
cramfs_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaCram";
	}
	if(vspopen_drain("mkcramfs -v -E -n \"%s\" %s",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
vfat_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// allow -c (badblock check) FIXME
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaVFAT";
	}
	if(vspopen_drain("mkfs.vfat %s-F 32 -n \"%s\" %s",
				mkm->force ? "-I " : "",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
ufs_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// allow -E (erase content for SSD)
	// allow -J (journaling)
	// allow -O1 (UFS1)
	// allow -U (soft updates)
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaUFS";
	}
	if(vspopen_drain("mkfs.ufs -L \"%s\" %s",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
ext4_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// pass -M with mount point FIXME
	// allow a UUID to be supplied FIXME
	// provide -o SprezzOS (and get it recognized rather than rejected) FIXME
	// allow -c (badblock check) FIXME
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaEXT4";
	}
	// FIXME Support a thorough mode or something where we use:
	// -E lazy_itable_init=0,lazy_journal_init=0 -O ^uninit_bg" or something
	if(mkm->stride && mkm->swidth){
		if(vspopen_drain("mkfs.ext4 -Estride=%ju,stripe_width=%ju %s-b -2048 -L \"%s\" -O dir_index,extent %s",
			mkm->stride,mkm->swidth,
			mkm->force ? "-F " : "",mkm->name,dev)){
		}
	}else if(vspopen_drain("mkfs.ext4 %s-b -2048 -L \"%s\" -O dir_index,extent %s",
			mkm->force ? "-F " : "",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
ext3_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// pass -M with mount point FIXME
	// allow a UUID to be supplied FIXME
	// provide -o SprezzOS (and get it recognized rather than rejected) FIXME
	// allow -c (badblock check) FIXME
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaEXT3";
	}
	//if(vspopen_drain("mkfs.ext3 %s-b -2048 -E lazy_itable_init=0,lazy_journal_init=0 -L \"%s\" -O dir_index,extent %s",
	if(mkm->stride && mkm->swidth){
		if(vspopen_drain("mkfs.ext3 -Estride=%ju,stripe_width=%ju %s-b -2048 -L \"%s\" -O dir_index,extent %s",
			mkm->stride,mkm->swidth,
			mkm->force ? "-F ": "",mkm->name,dev)){
		}
	}else if(vspopen_drain("mkfs.ext3 %s-b -2048 -L \"%s\" -O dir_index,extent %s",
			mkm->force ? "-F ": "",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
ext2_mkfs(const char *dev,const struct mkfsmarshal *mkm){
	// pass -M with mount point FIXME
	// allow a UUID to be supplied FIXME
	// provide -o SprezzOS (and get it recognized rather than rejected) FIXME
	// allow -c (badblock check) FIXME
	const char *name = mkm->name;

	if(name == NULL){
		name = "SprezzaEXT2";
	}
	if(mkm->stride && mkm->swidth){
		if(vspopen_drain("mkfs.ext2 -Estride=%ju,stripe_width=%ju %s-b -2048 -L \"%s\" -O dir_index,extent %s",
			mkm->stride,mkm->swidth,
			mkm->force ? "-F " : "",mkm->name,dev)){
		}
	}else if(vspopen_drain("mkfs.ext2 %s-b -2048 -L \"%s\" -O dir_index,extent %s",
			mkm->force ? "-F " : "",mkm->name,dev)){
		return -1;
	}
	return 0;
}

static int
mkswap(const char *dev,const struct mkfsmarshal *mfm){
	const char *name;

	name = mfm->name ? mfm->name : "SprezzaSwap";
	if(vspopen_drain("mkswap -L %s %s",name,dev)){
		return -1;
	}
	if(swapon(dev,0)){
		diag("Couldn't swap on %s (%s?)\n",dev,strerror(errno));
		return -1;
	}
	return 0;
}

// FIXME surely there's a less grotesque way of doing this? parse blkid -k?
static const struct fs {
	const char *name;
	const char *desc;
	int (*mkfs)(const char *,const struct mkfsmarshal *);
	int (*uuidset)(const char *,const unsigned char *);
	char nameparam;			// parameter on cmdline to name
	int namemax;			// max length of name, if known
} fss[] = {
	{
		.name = "vfat",
		.desc = "File Allocation Table (DOS default)",
		.mkfs = vfat_mkfs,
		.namemax = 11,
		.nameparam = 'n',
		.uuidset = NULL,
	},
	{
		.name = "swsuspend",
		.desc = "Software suspend block device",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "swap",
		.desc = "Swap device",
		.nameparam = 'L',
		.mkfs = mkswap,
		.uuidset = NULL, // FIXME
	},
	{
		.name = "xfs",
		.desc = "SGI's XFS (IRIX default)",
		.mkfs = xfs_mkfs,
		.namemax = 12,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ext4dev",
		.desc = "Obsolete, development-series ext4",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ext4",
		.desc = "Extended Filesystem v4 (Linux default)",
		.mkfs = ext4_mkfs,
		.namemax = 16,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ext3",
		.desc = "Extended Filesystem v3",
		.mkfs = ext3_mkfs,
		.namemax = 16,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ext2",
		.desc = "Extended Filesystem v2",
		.mkfs = ext2_mkfs,
		.namemax = 16,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "jbd",
		.desc = "Journaling Block Device",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "reiserfs",
		.desc = "ReiserFS v3",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "reiser4",
		.desc = "ReiserFS v4",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "jfs",
		.desc = "IBM's Journaled Filesystem (AIX JFS2)",
		.mkfs = jfs_mkfs,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "udf",
		.desc = "Universal Disk Format (ISO/IEC 13346)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "iso9660",
		.desc = "Compact Disc Filesystem (ISO 9660:1999)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ozfs",
		.desc = "Oracle's ZFS (Solaris 11 default)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "zol",
		.desc = "LLNL's ZoL (ZFS on Linux)",
		.mkfs = make_zfs,
		.nameparam = ' ',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "zfs_member",
		.desc = "ZFS zpool member",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "hfsplus",
		.desc = "HFS+ (Mac OS Extended) (OS X default)",
		.mkfs = hfsplus_mkfs,
		.nameparam = 'v',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "hfs",
		.desc = "Hierarchal Filesystem (Mac OS Standard)",
		.mkfs = hfs_mkfs,
		.nameparam = 'v',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ufs",
		.desc = "UNIX Filesystem 2 (BFFS) (BSD default)",
		.nameparam = 'L',
		.mkfs = ufs_mkfs,
		.uuidset = NULL, // FIXME

	},
	{
		.name = "hpfs",
		.desc = "OS/2's High Performance Filesystem",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "sysv",
		.desc = "System V Filesystem (S5FS) (XENIX default)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ntfs",
		.desc = "Microsoft's New Technology Filesystem (Windows default)",
		.mkfs = create_ntfs,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "cramfs",
		.desc = "Compressed Read-Only Filesystem",
		.mkfs = cramfs_mkfs,
		.nameparam = 'n',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "romfs",
		.desc = "Read-Only Filesystem",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "minix",
		.desc = "MINIX Filesystem (MINIX default)",
		.uuidset = NULL, // FIXME
       	},
	{
		.name = "gfs",
		.desc = "Red Hat's Global Filesystem v1",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "gfs2",
		.desc = "Red Hat's Global Filesystem v2",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ocfs",
		.desc = "Oracle Cluster Filesystem v1",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ocfs2",
		.desc = "Oracle Cluster Filesystem v2",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "oracleasm",
		.desc = "Oracle Automatic Storage Management",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "vxfs",
		.desc = "VERITAS Filesystem (HP-UX JFS) (HP-UX default)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "squashfs",
		.desc = "Squashed Read-Only Filesystem",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "nss",
		.desc = "Novell Storage Services",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "btrfs",
		.desc = "Oracle's B-Tree Filesystem",
		.nameparam = 'L',
		.mkfs = create_btrfs,
		.uuidset = NULL, // FIXME
	},
	{
		.name = "ubifs",
		.desc = "Unsorted Block Image Filesystem",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "bfs",
		.desc = "UNIXWare Boot Filesystem (SCO boot default)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "VMFS",
		.desc = "VMware's Virtual Machine Filesystem",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "befs",
		.desc = "Be Filesystem (BeOS default)",
		.uuidset = NULL, // FIXME
	},
	{
		.name = "nilfs2",
		.desc = "NTT's New Implementation of Log-structued FS v2",
		.namemax = 80,
		.nameparam = 'L',
		.uuidset = NULL, // FIXME
	},
	{
		.name = "exfat",
		.desc = "Microsoft's Extented File Allocation Table",
		.uuidset = NULL, // FIXME
	},
	{ .name = NULL, }
};

static struct fs virtfss[] = {
	{
		.name = "devpts",
		.desc = "Pseudoterminal devices",
	},{
		.name = "devtmpfs",
		.desc = "Early static /dev filesystem",
	},{
		.name = "nfs",
		.desc = "Network File System",
	},{
		.name = "nfs4",
		.desc = "Network File System v4",
	},{
		.name = "rootfs",
		.desc = "Early trivial filesystem mounted at /",
	},{
		.name = "proc",
		.desc = "Process information",
	},{
		.name = "sysfs",
		.desc = "Linux device/driver relationships",
	},{
		.name = "debugfs",
		.desc = "Linux free-form output filesystem",
	},{
		.name = "tmpfs",
		.desc = "RAM-backed filesystem",
	},{
		.name = "binfmt_misc",
		.desc = "Executable filetype management",
	},{
		.name = "nfsd",
		.desc = "Network filesystem daemon management",
	},{
		.name = "rpc_pipefs",
		.desc = "Remote Procedure Call pipe filesytem",
	},{
		.name = "fusectl",
		.desc = "Filesystem in UserSpacE management",
	},{
		.name = "fuse.fuseiso",
		.desc = "FUSE ISO9660 support",
	},{
		.name = "fuse.fuseiso9660",
		.desc = "FUSE ISO9660 support",
	},{
		.name = NULL,
	}
};

pttable_type *get_fs_types(int *count){
	pttable_type *pt;
	int z,t;

	*count = (sizeof(fss) / sizeof(*fss)) - 1;
	if(*count <= 0){
		diag("Invalid fs type count (%d), aborting",*count);
		return NULL;
	}
	if((pt = malloc(sizeof(*pt) * *count)) == NULL){
		*count = 0;
		return NULL;
	}
	t = 0;
	for(z = 0 ; z < *count ; ++z){
		if(fss[z].mkfs == NULL){
			continue;
		}
		if((pt[t].name = strdup(fss[z].name)) == NULL){
			goto err;
		}
		if(fss[z].desc == NULL){
			pt[t].desc = strdup("FIXME Need description");
		}else{
			if((pt[t].desc = strdup(fss[z].desc)) == NULL){
				free(pt[t].name);
				goto err;
			}
		}
		++t;
	}
	*count = t;
	return pt;

err:
	while(z--){
		free(pt[z].name);
		free(pt[z].desc);
	}
	free(pt);
	*count = 0;
	return NULL;
}

int make_filesystem(device *d,const char *ptype,const char *name){
	const struct fs *pt;
	int force = 0;

	if(d == NULL || ptype == NULL){
		diag("Passed NULL arguments, aborting\n");
		return -1;
	}
	if(d->mnttype){
		diag("Won't create fs on %s filesystem at %s\n",
				d->mnttype,d->name);
		return -1;
	}
	if(d->swapprio >= SWAP_MAXPRIO){
		diag("Won't create fs on active swap %s\n",d->name);
		return -1;
	}
	if(d->layout != LAYOUT_PARTITION){
		if(d->parts == NULL){
			force = 1;
		}
	}
	if(name){
		if(strchr(name,'"')){
			diag("Illegal character '\"' in name '%s'\n",name);
			return -1;
		}
	}
	for(pt = fss ; pt->name ; ++pt){
		if(strcmp(pt->name,ptype) == 0){
			char dbuf[PATH_MAX],*mnttype;
			struct mkfsmarshal marsh;

			memset(&marsh,0,sizeof(marsh));
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
			// FIXME needs accept/set UUID!
			marsh.name = name;
			marsh.force = force;
			if(d->layout == LAYOUT_MDADM){
				marsh.stride = d->mddev.stride;
				marsh.swidth = d->mddev.swidth;
			}
			if(pt->mkfs(dbuf,&marsh)){
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

int wipe_filesystem(device *d){
	if(!d->mnttype){
		diag("No filesystem on %s\n",d->name);
		return -1;
	}
	if(d->mnt.count){
		diag("%s is in use (%ux) and cannot be wiped\n",d->name,d->mnt.count);
		return -1;
	}
	if(vspopen_drain("wipefs -a /dev/%s",d->name)){
		return -1;
	}
	rescan_blockdev(d);
	return 0;
}

int fstype_uuid_p(const char *fstype){
	const struct fs *pt;

	for(pt = fss ; pt->name ; ++pt){
		if(strcmp(pt->name,fstype) == 0){
			if(pt->uuidset){
				return 1;
			}
			return 0;
		}
	}
	diag("Unknown filesystem type: %s\n",fstype);
	return 0;
}

int fstype_named_p(const char *fstype){
	const struct fs *pt;

	for(pt = fss ; pt->name ; ++pt){
		if(strcmp(pt->name,fstype) == 0){
			if(pt->namemax){
				return pt->namemax;
			}else if(pt->nameparam){
				return 1;
			}
			return 0;
		}
	}
	diag("Unknown filesystem type: %s\n",fstype);
	return 0;
}

// Is the filesystem virtual (not backed by a single device entry)?
int fstype_virt_p(const char *fs){
	const struct fs *pt;

	for(pt = virtfss ; pt->name ; ++pt){
		if(strcmp(pt->name,fs) == 0){
			return 1;
		}
	}
	return 0;
}

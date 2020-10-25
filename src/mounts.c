// copyright 2012–2020 nick black
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/statvfs.h>

#include "fs.h"
#include "zfs.h"
#include "mmap.h"
#include "mounts.h"
#include "growlight.h"
#include "aggregate.h"

static int
make_parent_directories(const char *path){
	char dir[PATH_MAX + 1];
	char *next;

	assert(strlen(path) < sizeof(dir));
	strcpy(dir,path);
	next = dir;
	while(*next && (next = strchr(next,'/')) ){
		if(next == dir){
			++next;
			continue;
		}
		*next = '\0';
		if(mkdir(dir,0755) && errno != EEXIST){
			diag("Couldn't create directory at %s (%s?)\n",dir,strerror(errno));
			return -1;
		}
		*next = '/';
		++next;
	}
	if(mkdir(dir,0755) && errno != EEXIST){
		diag("Couldn't create directory at %s (%s?)\n",dir,strerror(errno));
		return -1;
	}
	return 0;
}

static int
parse_mount(const char *map,off_t len,char **dev,char **mnt,char **fs,char **ops){
	const char *t;
	int r = 0;

	*dev = *mnt = *fs = *ops = NULL;
	t = map;
	if(len <= t - map){
		goto err;
	}
	while(isgraph(map[r])){
		if(++r >= len){
			goto err;
		}
	}
	if(!isspace(map[r])){
		goto err;
	}
	if(r == t - map){
		goto err;
	}
	if((*dev = strndup(t,r - (t - map))) == NULL){
		goto err;
	}
	t = map + ++r;
	if(len <= t - map){
		goto err;
	}
	while(isgraph(map[r])){
		if(++r >= len){
			goto err;
		}
	}
	if(!isspace(map[r])){
		goto err;
	}
	if(r == t - map){
		goto err;
	}
	if((*mnt = strndup(t,r - (t - map))) == NULL){
		goto err;
	}
	t = map + ++r;
	if(len <= t - map){
		goto err;
	}
	while(isgraph(map[r])){
		if(++r >= len){
			goto err;
		}
	}
	if(!isspace(map[r])){
		goto err;
	}
	if(r == t - map){
		goto err;
	}
	if((*fs = strndup(t,r - (t - map))) == NULL){
		goto err;
	}
	t = map + ++r;
	if(len <= t - map){
		goto err;
	}
	while(isgraph(map[r])){
		if(++r >= len){
			goto err;
		}
	}
	if(!isspace(map[r])){
		goto err;
	}
	if(r == t - map){
		goto err;
	}
	if((*ops = strndup(t,r - (t - map))) == NULL){
		goto err;
	}
	while(r < len){
		if(map[r] == '\n'){
			break;
		}
		++r;
	}
	if(r >= len){
		goto err;
	}
	++r;
	return r;

err:
	diag("Couldn't extract mount info from %s\n",map);
	free(*dev);
	free(*mnt);
	free(*fs);
	free(*ops);
	return -1;
}

int parse_mounts(const glightui *gui, const char *fn){
	char *mnt, *dev, *ops, *fs;
	off_t len, idx;
	char *map;
	int fd;

	if((map = map_virt_file(fn, &fd, &len)) == MAP_FAILED){
		return -1;
	}
	idx = 0;
	dev = mnt = fs = ops = NULL;
	while(idx < len){
		char buf[PATH_MAX + 1];
		struct statvfs vfs;
		struct stat st;
		device *d;
		char *rp;
		int r;

		free(dev); free(mnt); free(fs); free(ops);
    dev = mnt = fs = ops = NULL; // don't goto double-free()
		if((r = parse_mount(map + idx, len - idx, &dev, &mnt, &fs, &ops)) < 0){
			goto err;
		}
		idx += r;
		if(statvfs(mnt, &vfs)){
			int skip = 0;

			// We might have mounted a new target atop or above an
			// already existing one,  in which case we'll need
			// possibly recreate the directory structure on the
			// newly-mounted filesystem.
			if(growlight_target){
				if(strncmp(mnt, growlight_target, strlen(growlight_target)) == 0){
					if(make_parent_directories(mnt) == 0){
						skip = 1;
					} // FIXME else remount? otherwise writes
					// go to new filesystem rather than old...?
				}
			}
			if(!skip){
				diag("Couldn't stat fs %s (%s?)\n", mnt, strerror(errno));
				continue;
			}
		}
		if(*dev != '/'){ // have to get zfs's etc
			if(fstype_virt_p(fs)){
				continue;
			}
			if((d = lookup_device(dev)) == NULL){
				verbf("virtfs %s at %s\n", fs, mnt);
				continue;
			}
		}else{
			rp = dev;
			if(lstat(rp, &st) == 0){
				if(S_ISLNK(st.st_mode)){
					if((r = readlink(dev, buf, sizeof(buf))) < 0){
						diag("Couldn't deref %s (%s?)\n", dev, strerror(errno));
						continue;
					}
					if((size_t)r >= sizeof(buf)){
						diag("Name too long for %s (%d?)\n", dev, r);
						continue;
					}
					buf[r] = '\0';
					rp = buf;
				}
			}
			if((d = lookup_device(rp)) == NULL){
				continue;
			}
		}
		free(dev);
		dev = NULL;
		if(d->mnttype && strcmp(d->mnttype, fs)){
			diag("Already had mounttype for %s: %s (got %s)\n",
					d->name, d->mnttype, fs);
			free(d->mnttype);
			d->mnttype = NULL;
			free_stringlist(&d->mntops);
			free_stringlist(&d->mnt);
			d->mnttype = fs;
		}else{
			free(fs);
		}
		fs = NULL;
		if(add_string(&d->mnt, mnt)){
			goto err;
		}
		if(add_string(&d->mntops, ops)){
			goto err;
		}
		d->mntsize = (uintmax_t)vfs.f_bsize * vfs.f_blocks;
		if(d->layout == LAYOUT_PARTITION){
			d = d->partdev.parent;
		}
		d->uistate = gui->block_event(d, d->uistate);
		if(growlight_target){
			if(strcmp(mnt, growlight_target) == 0){
				mount_target();
			}
		}
	}
	free(dev); free(mnt); free(fs); free(ops);
	dev = mnt = fs = ops = NULL;
	munmap_virt(map, len);
	close(fd);
	return 0;

err:
	munmap_virt(map, len);
	close(fd);
	return -1;
}

int mmount(device *d, const char *targ, unsigned mntops, const void *data){
	char name[PATH_MAX + 1];
	char *rname;

	if(d == NULL || targ == NULL){ // mntops may be NULL
		diag("Provided NULL arguments\n");
		return -1;
	}
	if(!d->mnttype){
		diag("%s does not have a filesystem signature\n", d->name);
		return -1;
	}
	if(strcmp(d->mnttype, "zfs") == 0){
		return mount_zfs(d, targ, mntops, data);
	}
	if(mnttype_aggregablep(d->mnttype)){
		diag("not a mountable filesystem: %s \n", d->mnttype);
		return -1;
	}
	if(growlight_target){
		if(strncmp(targ, growlight_target, strlen(growlight_target)) == 0){
			if(make_parent_directories(targ)){
				diag("Couldn't make parents of %s\n", targ);
			}
		}
	}
	if((rname = realpath(targ, NULL)) == NULL){
		diag("Couldn't canonicalize %s (%s)\n", targ, strerror(errno));
		return -1;
	}
	if(string_included_p(&d->mnt, rname)){
		diag("%s is already mounted at %s\n", d->name, targ);
		free(rname);
		return -1;
	}
	if(growlight_target){
		if(strncmp(rname, growlight_target, strlen(growlight_target)) == 0){
			if(make_parent_directories(rname)){
				diag("Couldn't make parents of %s\n", rname);
			}
		}
	}
	snprintf(name, sizeof(name), "/dev/%s", d->name);
	// Use the original path for the actual mount
	if(mount(name, targ, d->mnttype, mntops, data)){
		diag("Error mounting %s (%u) at %s (%s?)\n",
				name, mntops, targ, strerror(errno));
		free(rname);
		return -1;
	}
	diag("Mounted %s at %s\n", d->name, targ);
	free(rname);
	return 0;
}

int unmount(device *d, const char *path){
	unsigned z;

	if(d->mnt.count == 0){
		diag("%s is not mounted\n", d->name);
		return -1;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(path && strcmp(d->mnt.list[z], path) == 0){
			continue;
		}
		diag("Unmounting %s from %s\n", d->name, d->mnt.list[z]);
		if(growlight_target){
      if(strcmp(d->mnt.list[z], growlight_target) == 0){
			  unmount_target();
		  }
    }
		if(umount2(d->mnt.list[z], UMOUNT_NOFOLLOW)){
			diag("Error unmounting %s at %s (%s?)\n",
					d->name, d->mnt.list[z], strerror(errno));
			return -1;
		}
	}
	return 0;
}

void clear_mounts(controller *c){
	unmount_target();
	while(c){
		device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			device *p;

			// Don't free mnttype. There's still a filesystem.
			free_stringlist(&d->mnt);
			free_stringlist(&d->mntops);
			for(p = d->parts ; p ; p = p->next){
				free_stringlist(&p->mnt);
				free_stringlist(&p->mntops);
			}
		}
		c = c->next;
	}
}

unsigned flag_for_mountop(const char *op){
	struct opmap {
		const char *o;
		unsigned v;
	} map[] = {
		{
			.o = "ro",
			.v = MS_RDONLY,
		},{
			.o = "dirsync",
			.v = MS_DIRSYNC,
		},{
			.o = "mand",
			.v = MS_MANDLOCK,
		},{
			.o = "noatime",
			.v = MS_NOATIME,
		},{
			.o = "nodev",
			.v = MS_NODEV,
		},{
			.o = "nodiratime",
			.v = MS_NODIRATIME,
		},{
			.o = "noexec",
			.v = MS_NOEXEC,
		},{
			.o = "nosuid",
			.v = MS_NOSUID,
		},{
			.o = "ro",
			.v = MS_RDONLY,
		},{
			.o = "relatime",
			.v = MS_RELATIME,
		},{
			.o = "silent",
			.v = MS_SILENT,
		},{
			.o = "strictatime",
			.v = MS_STRICTATIME,
		},{
			.o = "sync",
			.v = MS_SYNCHRONOUS,
		},{
			.o = NULL,
			.v = 0,
		}
	},*cur;

	for(cur = map ; cur->o ; ++cur){
		if(strcmp(cur->o,op) == 0){
			return cur->v;
		}
	}
	return 0;
}

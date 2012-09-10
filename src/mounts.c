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
#include "mmap.h"
#include "mounts.h"
#include "growlight.h"

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
	t = map + ++r;
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

int parse_mounts(const glightui *gui,const char *fn){
	char *mnt,*dev,*ops,*fs;
	off_t len,idx;
	char *map;
	int fd;

	if((map = map_virt_file(fn,&fd,&len)) == MAP_FAILED){
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
		if((r = parse_mount(map + idx,len - idx,&dev,&mnt,&fs,&ops)) < 0){
			goto err;
		}
		idx += r;
		if(statvfs(mnt,&vfs)){
			diag("Couldn't stat fs %s (%s?)\n",dev,strerror(errno));
			return -1;
		}
		if(*dev != '/'){ // have to get zfs's etc
			if(fstype_virt_p(fs)){
				continue;
			}
			if((d = lookup_device(dev)) == NULL){
				verbf("virtfs %s at %s\n",fs,mnt);
				continue;
			}
		}else{
			rp = dev;
			if(lstat(rp,&st) == 0){
				if(S_ISLNK(st.st_mode)){
					if((r = readlink(dev,buf,sizeof(buf))) < 0){
						diag("Couldn't deref %s (%s?)\n",dev,strerror(errno));
						goto err;
					}
					if((size_t)r >= sizeof(buf)){
						diag("Name too long for %s (%d?)\n",dev,r);
						goto err;
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
		if(d->mnttype && strcmp(d->mnttype,fs) == 0){
			diag("Already had mounttype for %s: %s (got %s)\n",
					d->name,d->mnttype,fs);
			free(d->mnttype);
			d->mnttype = NULL;
			free_stringlist(&d->mntops);
			free_stringlist(&d->mnt);
			d->mnttype = fs;
		}else{
			free(fs);
		}
		fs = NULL;
		if(add_string_exclusive(&d->mnt,mnt)){
			goto err;
		}
		if(add_string_exclusive(&d->mntops,ops)){
			goto err;
		}
		d->mntsize = (uintmax_t)vfs.f_bsize * vfs.f_blocks;
		if(d->layout == LAYOUT_PARTITION){
			d = d->partdev.parent;
		}
		d->uistate = gui->block_event(d,d->uistate);
	}
	free(mnt); free(fs); free(ops);
	mnt = fs = ops = NULL;
	munmap_virt(map,len);
	close(fd);
	return 0;

err:
	free(dev); free(mnt); free(fs); free(ops);
	munmap_virt(map,len);
	close(fd);
	return -1;
}

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

int mmount(device *d,const char *targ){
	char name[PATH_MAX + 1];

	if(d == NULL || targ == NULL){
		diag("Provided NULL arguments\n");
		return -1;
	}
	if(!d->mnttype){
		diag("%s does not have a filesystem signature\n",d->name);
		return -1;
	}
	if(string_included_p(&d->mnt,targ)){
		diag("%s is already mounted at %s\n",d->name,targ);
		return -1;
	}
	if(growlight_target){
		if(strncmp(targ,growlight_target,strlen(growlight_target)) == 0){
			if(make_parent_directories(targ)){
				diag("Couldn't make parents of %s\n",targ);
			}
		}
	}
	snprintf(name,sizeof(name),"/dev/%s",d->name);
	if(mount(name,targ,d->mnttype,MS_NOATIME,NULL)){
		diag("Error mounting %s at %s (%s?)\n",
				name,targ,strerror(errno));
		return -1;
	}
	if(growlight_target){
		if(strcmp(targ,growlight_target) == 0){
			mount_target();
		}
	}
	diag("Mounted %s at %s\n",d->name,targ);
	return 0;
}

int unmount(device *d,const char *path){
	unsigned z;

	if(d->mnt.count == 0){
		diag("%s is not mounted\n",d->name);
		return -1;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(path && strcmp(d->mnt.list[z],path) == 0){
			continue;
		}
		diag("Unmounting %s from %s\n",d->name,d->mnt.list[z]);
		if(strcmp(d->mnt.list[z],growlight_target) == 0){
			unmount_target();
		}
		if(umount2(d->mnt.list[z],UMOUNT_NOFOLLOW)){
			diag("Error unmounting %s at %s (%s?)\n",
					d->name,d->mnt.list[z],strerror(errno));
			return -1;
		}
	}
	return 0;
}

void clear_mounts(controller *c){
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

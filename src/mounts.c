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
		if(d->mnt){
			if(strcmp(d->mnt,mnt)){
				diag("Already had mount for %s|%s: %s|%s\n",
						dev,mnt,d->name,d->mnt);
				// FIXME need to track both! see bug 175
				free(d->mnttype);
				free(d->mntops);
				free(d->mnt);
			}
		}else{
			verbf("New %s mount %s on %s\n",fs,mnt,d->name);
		}
		d->mnt = mnt;
		d->mntops = ops;
		d->mnttype = fs;
		d->mntsize = (uintmax_t)vfs.f_bsize * vfs.f_blocks;
		if(d->layout == LAYOUT_PARTITION){
			d = d->partdev.parent;
		}
		d->uistate = gui->block_event(d,d->uistate);
		mnt = fs = ops = NULL;
	}
	free(dev); free(mnt); free(fs); free(ops);
	dev = mnt = fs = ops = NULL;
	munmap_virt(map,len);
	close(fd);
	return 0;

err:
	free(dev); free(mnt); free(fs); free(ops);
	munmap_virt(map,len);
	close(fd);
	return -1;
}

int mmount(device *d,const char *targ,const char *fs){
	char name[PATH_MAX + 1];

	if(d == NULL || targ == NULL || fs == NULL){
		diag("Provided NULL arguments\n");
		return -1;
	}
	if(d->mnt){
		diag("%s is already mounted\n",d->name);
		return -1;
	}
	if((d->mnt = strdup(fs)) == NULL){
		return -1;
	}
	snprintf(name,sizeof(name),"/dev/%s",d->name);
	if(mount(name,targ,fs,MS_NOATIME,NULL)){
		diag("Error mounting %s at %s (%s?)\n",
				name,targ,strerror(errno));
		free(d->mnt);
		d->mnt = NULL;
		return -1;
	}
	diag("Mounted %s at %s\n",d->name,d->mnt);
	return 0;
}

int unmount(device *d){
	if(d->mnt == NULL){
		diag("%s is not mounted\n",d->name);
		return -1;
	}
	if(umount2(d->mnt,UMOUNT_NOFOLLOW)){
		diag("Error unmounting %s at %s (%s?)\n",
				d->name,d->mnt,strerror(errno));
		return -1;
	}
	printf("Unmounted %s from %s\n",d->name,d->mnt);
	free(d->mnt);
	d->mnt = NULL;
	return 0;
}

void clear_mounts(controller *c){
	while(c){
		device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			device *p;

			// Don't free mnttype. There's still a filesystem.
			free(d->mnt);
			free(d->mntops);
			d->mnt = d->mntops = NULL;
			for(p = d->parts ; p ; p = p->next){
				free(p->mnt);
				free(p->mntops);
				p->mnt = p->mntops = NULL;
			}
		}
		c = c->next;
	}
}

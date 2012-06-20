#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

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
	fprintf(stderr,"Couldn't extract mount info from %s\n",map);
	free(*dev);
	free(*mnt);
	free(*fs);
	free(*ops);
	return -1;
}

int parse_mounts(const char *fn){
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
		char buf[PATH_MAX + 1],*rp;
		struct stat st;
		device *d;
		int r;

		free(dev); free(mnt); free(fs); free(ops);
		if((r = parse_mount(map + idx,len - idx,&dev,&mnt,&fs,&ops)) < 0){
			goto err;
		}
		idx += r;
		if(dev[0] != '/'){
			continue;
		}
		if(lstat(dev,&st)){
			fprintf(stderr,"Couldn't stat %s (%s?)\n",dev,strerror(errno));
			goto err;
		}
		if(S_ISLNK(st.st_mode)){
			int r;

			if((r = readlink(dev,buf,sizeof(buf))) < 0){
				fprintf(stderr,"Couldn't deref %s (%s?)\n",dev,strerror(errno));
				goto err;
			}
			if((size_t)r >= sizeof(buf)){
				fprintf(stderr,"Name too long for %s (%d?)\n",dev,r);
				goto err;
			}
			buf[r] = '\0';
			rp = buf;
		}else{
			rp = dev;
		}
		if((d = lookup_device(rp)) == NULL){
			goto err;
		}
		/*if((d = lookup_dentry(d,rp)) == NULL){
			fprintf(stderr,"Couldn't find device %s\n",rp);
			goto err;
		}*/
		if(d->mnt){
			fprintf(stderr,"Already had mount for %s|%s: %s|%s\n",
					dev,mnt,d->name,d->mnt);
			goto err;
		}
		d->mnt = mnt;
		d->mnttype = fs;
		d->mntops = ops;
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

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "popen.h"
#include "target.h"
#include "growlight.h"

// Path on the guest filesystem which will hold the target's root filesystem.
const char *growlight_target = NULL;
char real_target[PATH_MAX + 1]; // Only used when we set or unset the target

static int targfd = -1; // reference to target root, once defined

static void
use_new_target(const char *path){
	const controller *c;

	for(c = get_controllers() ; c ; c = c->next){
		device *d,*p;

		for(d = c->blockdevs ; d ; d = d->next){
			for(p = d->parts ; p ; p = p->next){
				if(string_included_p(&p->mnt,path)){
					mount_target();
				}
			}
			if(string_included_p(&d->mnt,path)){
				mount_target();
			}
		}
	}
}

int set_target(const char *path){
	if(path){
		if(growlight_target){
			diag("A target is already defined: %s\n",growlight_target);
			return -1;
		}
		if(realpath(path,real_target) == NULL){
			diag("Couldn't resolve %s (%s?)\n",path,strerror(errno));
			return -1;
		}
		use_new_target(real_target);
		growlight_target = real_target;
		return 0;
	}
	if(!growlight_target){
		diag("No target is defined\n");
		return -1;
	}
	growlight_target = NULL;
	if(targfd >= 0){
		close(targfd);
		targfd = -1;
	}
	return 0;
}

int finalize_target(void){
	char pathext[PATH_MAX + 1],*fstab;
	FILE *fp;
	int fd,r;

	if(!growlight_target){
		diag("No target is defined\n");
		return -1;
	}
	if(targfd < 0){
		diag("No target mappings are defined\n");
		return -1;
	}
	if((unsigned)snprintf(pathext,sizeof(pathext),"%s/etc",growlight_target) >= sizeof(pathext)){
		diag("Name too long (%s/etc)\n",growlight_target);
		return -1;
	}
	if(mkdir(pathext,0755) && errno != EEXIST){
		diag("Couldn't mkdir %s (%s?)\n",pathext,strerror(errno));
		return -1;
	}
	if((fd = openat(targfd,"etc/fstab",O_WRONLY|O_CLOEXEC|O_CREAT,
					S_IROTH | S_IRGRP | S_IWGRP | S_IRUSR | S_IWUSR)) < 0){
		diag("Couldn't open etc/fstab in target root (%s?)\n",strerror(errno));
		return -1;
	}
	if((fp = fdopen(fd,"w")) == NULL){
		diag("Couldn't get FILE * from %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
	}
	if((fstab = dump_targets()) == NULL){
		diag("Couldn't write targets to %s/etc/fstab (%s?)\n",growlight_target,strerror(errno));
		close(fd);
		return -1;
	}
	if((r = fprintf(fp,"%s",fstab)) < 0 || (size_t)r < strlen(fstab)){
		diag("Couldn't write data to %s/etc/fstab (%s?)\n",growlight_target,strerror(errno));
		free(fstab);
		close(fd);
		return -1;
	}
	free(fstab);
	if(fclose(fp)){
		diag("Couldn't close FILE * from %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
	}
	finalized = 1;
	return 0;
}

// FIXME prefer labels or UUIDs for identification!
static const char *
fstab_name(const device *d){
	return d->name;
}

static char *
dump_device_targets(char *s,const device *d){
	unsigned z;
	char *tmp;
	int r;

	// ZFS maintains its own mountpoint tracking, external to /etc/fstab
	if(strcmp(d->mnttype,"zfs") == 0){
		return 0;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(strncmp(d->mnt.list[z],growlight_target,strlen(growlight_target)) == 0){
			continue;
		}
		r = snprintf(NULL,0,"/dev/%s\t%s\t\t%s\t%s\t0\t%u\n",fstab_name(d),
				d->mnt.list[z] + strlen(growlight_target) -
				 !strcmp(d->mnt.list[z],growlight_target),
				d->mnttype,d->mntops.list[z],
				strcmp(d->mnt.list[z],growlight_target) ? 2 : 1);
		if((tmp = realloc(s,sizeof(*s) * (strlen(s) + r + 1))) == NULL){
			goto err;
		}
		s = tmp;
		sprintf(s + strlen(s),"/dev/%s\t%s\t\t%s\t%s\t0\t%u\n",fstab_name(d),
				d->mnt.list[z] + strlen(growlight_target) -
				 !strcmp(d->mnt.list[z],growlight_target),
				d->mnttype,d->mntops.list[z],
				strcmp(d->mnt.list[z],growlight_target) ? 2 : 1);
	}
	if(d->swapprio != SWAP_INVALID){
		r = snprintf(NULL,0,"/dev/%s\tnone\t\t%s\n",fstab_name(d),d->mnttype);
		if((tmp = realloc(s,sizeof(*s) * (strlen(s) + r + 1))) == NULL){
			goto err;
		}
		s = tmp;
		sprintf(s + strlen(s),"/dev/%s\tnone\t\t%s\n",fstab_name(d),d->mnttype);
	}
	return s;

err:
	return NULL;
}

char *dump_targets(void){
	char *out = NULL,*tmp;
	const controller *c;
	size_t off = 0;
	int z;

	if(targfd < 0){
		return 0;
	}
	// FIXME allow various naming schemes
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(d->layout == LAYOUT_NONE && d->blkdev.removable){
				// FIXME differentiate USB etc
				z = snprintf(NULL,0,"/dev/%s\t%s\t%s\t%s\t0\t0\n",fstab_name(d),
						"/media/cdrom","auto","noauto,user");
				if((tmp = realloc(out,sizeof(*out) * (z + off + 1))) == NULL){
					goto err;
				}
				out = tmp;
				sprintf(out + off,"/dev/%s\t%s\t%s\t%s\t0\t0\n",fstab_name(d),
						"/media/cdrom","auto","noauto,user");
				off += z;
			}else{
				if((tmp = dump_device_targets(out,d)) == NULL){
					goto err;
				}
				out = tmp;
				off += strlen(out + off);
			}
			for(p = d->parts ; p ; p = p->next){
				if((tmp = dump_device_targets(out,p)) == NULL){
					goto err;
				}
				out = tmp;
				off += strlen(out + off);
			}
		}
	}
#define PROCLINE "proc\t\t/proc\t\tproc\tdefaults\t0\t0\n"
	if((tmp = realloc(out,sizeof(*out) * (off + strlen(PROCLINE) + 1))) == NULL){
		goto err;
	}
	out = tmp;
	sprintf(out + off,"%s",PROCLINE);
	off += strlen(PROCLINE);
#undef PROCLINE
	return out;

err:
	free(out);
	return NULL;
}

int mount_target(void){
	if(targfd >= 0){
		verbf("Targfd already opened at %d\n",targfd);
		close(targfd);
	}
	if((targfd = open(growlight_target,O_DIRECTORY|O_RDONLY|O_CLOEXEC)) < 0){
		diag("Couldn't open %s (%s?)\n",growlight_target,strerror(errno));
		return -1;
	}
	return 0;
}

int unmount_target(void){
	if(targfd < 0){
		verbf("Targfd wasn't open\n");
		return -1;
	}
	if(close(targfd)){
		diag("Error closing targfd %d (%s)\n",targfd,strerror(errno));
		targfd = -1;
		return -1;
	}
	targfd = -1;
	return 0;
}

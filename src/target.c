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
	char pathext[PATH_MAX + 1];
	FILE *fp;
	int fd;

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
	if(dump_targets(fp)){
		diag("Couldn't write targets to %s/etc/fstab (%s?)\n",growlight_target,strerror(errno));
		close(fd);
		return -1;
	}
	if(fclose(fp)){
		diag("Couldn't close FILE * from %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
	}
	finalized = 1;
	return 0;
}

static int
dump_device_targets(const device *d,FILE *fp){
	unsigned z;

	for(z = 0 ; z < d->mnt.count ; ++z){
		if(strncmp(d->mnt.list[z],growlight_target,strlen(growlight_target)) == 0){
			if(fprintf(fp,"/dev/%s\t%s\t\t%s\t%s\t0\t%u\n",d->name,
					d->mnt.list[z] + strlen(growlight_target) - 1,
					d->mnttype,d->mntops.list[z],
					strcmp(d->mnt.list[z],growlight_target) ? 2 : 1) < 0){
				return -1;
			}
		}
	}
	return 0;
}

int dump_targets(FILE *fp){
	const controller *c;

	if(targfd < 0){
		return 0;
	}
	// FIXME allow various naming schemes
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(dump_device_targets(d,fp)){
				return -1;
			}
			if(d->layout == LAYOUT_NONE){
				if(d->blkdev.removable){
					if(fprintf(fp,"/dev/%s\t%s\t%s\t%s\t0\t0\n",d->name,
							"/media/cdrom","auto","noauto,user") < 0){
						return -1;
					}
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(dump_device_targets(p,fp)){
					return -1;
				}
			}
		}
	}
	if(fprintf(fp,"proc\t\t/proc\t\tproc\tdefaults\t0\t0\n") < 0){
		return -1;
	}
	return 0;
}

int mount_target(void){
	if(targfd >= 0){
		diag("Targfd already opened at %d\n",targfd);
		return -1;
	}
	if((targfd = open(growlight_target,O_DIRECTORY|O_RDONLY|O_CLOEXEC)) < 0){
		diag("Couldn't open %s (%s?)\n",growlight_target,strerror(errno));
		return -1;
	}
	return 0;
}

int unmount_target(void){
	if(targfd < 0){
		diag("Targfd wasn't open\n");
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

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "target.h"
#include "growlight.h"

// Path on the guest filesystem which will hold the target's root filesystem.
char *real_target; // Only used when we set or unset the target
const char *growlight_target = NULL;

static int targfd = -1; // reference to target root, once defined

static mntentry *
create_target(const char *path,const char *dev,const char *ops){
	mntentry *t;

	if( (t = malloc(sizeof(*t))) ){
		t->label = t->uuid = NULL;
		t->path = strdup(path);
		t->dev = strdup(dev);
		t->ops = strdup(ops);
		if(!t->path || !t->dev || !t->ops){
			free(t->path);
			free(t->dev);
			free(t->ops);
			free(t);
			t = NULL;
		}
	}
	if(!t){
		fprintf(stderr,"Failure creating fs on %s\n",dev);
	}
	return t;
}

void free_mntentry(mntentry *t){
	if(t){
		free(t->label);
		free(t->uuid);
		free(t->path);
		free(t->dev);
		free(t->ops);
		free(t);
	}
}

int prepare_mount(device *d,const char *path,const char *cfs,const char *ops){
	char devname[PATH_MAX + 1],pathext[PATH_MAX + 1],*fs;
	mntentry *m;

	if(get_target() == NULL){
		fprintf(stderr,"No target is defined\n");
		return -1;
	}
	if(d->mnt){
		fprintf(stderr,"%s is already actively mounted at %s\n",d->name,d->mnt);
		return -1;
	}
	if(d->target){
		fprintf(stderr,"%s is already mapped to %s\n",d->name,d->target->path);
		return -1;
	}
	if(d->swapprio >= SWAP_MAXPRIO){
		fprintf(stderr,"%s is used as swap\n",d->name);
		return -1;
	}
	if(snprintf(devname,sizeof(devname),"/dev/%s",d->name) >= (int)sizeof(devname)){
		fprintf(stderr,"Bad device name: %s\n",d->name);
		return -1;
	}
	if(snprintf(pathext,sizeof(pathext),"%s/%s",get_target(),path) >= (int)(sizeof(devname) - strlen("/etc/fstab"))){
		fprintf(stderr,"Bad mount point: %s\n",path);
		return -1;
	}
	if((fs = strdup(cfs)) == NULL){
		return -1;
	}
	if(targfd < 0){
		if(strcmp(path,"/")){
			fprintf(stderr,"Need a root ('/') before mapping %s\n",path);
			free(fs);
			return -1;
		}
		if(mount(devname,pathext,fs,MS_NOATIME,NULL)){
			fprintf(stderr,"Couldn't mount %s at %s for %s\n",devname,pathext,fs);
			free(fs);
			return -1;
		}
		// we know we have enough space from the check of snprintf()...
		if((targfd = open(pathext,O_DIRECTORY|O_RDONLY|O_CLOEXEC)) < 0){
			fprintf(stderr,"Couldn't open %s (%s?)\n",path,strerror(errno));
			free(fs);
			return -1;
		}
		strcat(pathext,"/etc");
		if(mkdir(pathext,0777) && errno != EEXIST){
			fprintf(stderr,"Couldn't mkdir %s (%s?)\n",pathext,strerror(errno));
			close(targfd);
			targfd = -1;
			umount2(devname,UMOUNT_NOFOLLOW);
			free(fs);
			return -1;
		}
		d->swapprio = SWAP_INVALID;
		if((d->target = create_target(path,d->name,ops)) == NULL){
			close(targfd);
			targfd = -1;
			free(fs);
			return -1;
		}
		free(d->mnttype);
		d->mnttype = fs;
		return 0;
	}
	// no need to check for preexisting mount at this point -- the mount(2)
	// will fail if one's there.
	if(mount(devname,path,fs,MS_NOATIME,NULL)){
		fprintf(stderr,"Couldn't mount %s at %s for %s\n",devname,path,fs);
		free(fs);
		return -1;
	}
	d->swapprio = SWAP_INVALID;
	if((m = create_target(path,d->name,ops)) == NULL){
		umount2(devname,UMOUNT_NOFOLLOW);
		free(fs);
		return -1;
	}
	free(d->mnttype);
	d->mnttype = fs;
	return 0;
}

int set_target(const char *path){
	const controller *c;

	if(path){
		if(growlight_target){
			fprintf(stderr,"A target is already defined: %s\n",growlight_target);
			return -1;
		}
		if((growlight_target = real_target = strdup(path)) == NULL){
			fprintf(stderr,"Couldn't set target (%s?)\n",strerror(errno));
			return -1;
		}
	}else if(!growlight_target){
		fprintf(stderr,"No target is defined\n");
		return -1;
	}
	for(c = get_controllers() ; c ; c = c->next){
		char buf[PATH_MAX + 1];
		device *d,*p;

		for(d = c->blockdevs ; d ; d = d->next){
			if(d->target){
				if(snprintf(buf,sizeof(buf),"%s/%s",growlight_target,d->target->path) >= (int)sizeof(buf)){
					fprintf(stderr,"Path too long: %s/%s\n",growlight_target,d->target->path);
				}else if(umount2(buf,UMOUNT_NOFOLLOW)){
					fprintf(stderr,"Couldn't unmount %s (%s?)\n",buf,strerror(errno));
				}
				free_mntentry(d->target);
				d->target = NULL;
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->target){
					if(snprintf(buf,sizeof(buf),"%s/%s",growlight_target,d->target->path) >= (int)sizeof(buf)){
						fprintf(stderr,"Path too long: %s/%s\n",growlight_target,d->target->path);
					}else if(umount2(buf,UMOUNT_NOFOLLOW)){
						fprintf(stderr,"Couldn't unmount %s (%s?)\n",buf,strerror(errno));
					}
					free_mntentry(d->target);
					d->target = NULL;
				}
			}
		}
	}
	free(real_target);
	growlight_target = real_target = NULL;
	close(targfd);
	targfd = -1;
	return 0;
}

int finalize_target(void){
	FILE *fp;
	int fd;

	if(!growlight_target){
		fprintf(stderr,"No target is defined\n");
		return -1;
	}
	if(targfd < 0){
		fprintf(stderr,"No target mappings are defined\n");
		return -1;
	}
	if((fd = openat(targfd,"etc/fstab",O_WRONLY|O_CLOEXEC|O_CREAT,
					S_IROTH | S_IRGRP | S_IWGRP | S_IRUSR | S_IWUSR)) < 0){
		fprintf(stderr,"Couldn't open etc/fstab in target root (%s?)\n",strerror(errno));
		return -1;
	}
	if((fp = fdopen(fd,"w")) == NULL){
		fprintf(stderr,"Couldn't get FILE * from %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
	}
	if(dump_targets(fp)){
		fprintf(stderr,"Couldn't write targets to %s/etc/fstab (%s?)\n",growlight_target,strerror(errno));
		close(fd);
		return -1;
	}
	if(fclose(fp)){
		fprintf(stderr,"Couldn't close FILE * from %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
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
			const mntentry *m;
			const device *p;

			if( (m = d->target) ){
				fprintf(fp,"/dev/%s\t%s\t\t%s\t%s\t0\t%u\n",m->dev,
						m->path,d->mnttype,m->ops,strcmp(m->path,"/") ? 2 : 1);
			}
			for(p = d->parts ; p ; p = p->next){
				if( (m = p->target) ){
					fprintf(fp,"/dev/%s\t%s\t\t%s\t%s\t0\t%u\n",m->dev,
							m->path,p->mnttype,m->ops,strcmp(m->path,"/") ? 2 : 1);
				}
			}
		}
	}
	fprintf(fp,"proc\t\t/proc\t\tproc\tdefaults\t0\t0\n");
	return 0;
}

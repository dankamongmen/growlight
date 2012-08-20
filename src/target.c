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

static mntentry *
create_target(const char *path,const char *dev,const char *uuid,
			const char *label,const char *ops){
	mntentry *t;

	if( (t = malloc(sizeof(*t))) ){
		t->path = strdup(path);
		t->dev = strdup(dev);
		t->ops = strdup(ops);
		t->uuid = uuid ? strdup(uuid) : NULL;
		t->label = label ? strdup(label) : NULL;
		if(!t->path || !t->dev || !t->ops || (label && !t->label)
						|| (uuid && !t->uuid)){
			free(t->label);
			free(t->uuid);
			free(t->path);
			free(t->dev);
			free(t->ops);
			free(t);
			t = NULL;
		}
	}
	if(!t){
		diag("Failure creating fs on %s\n",dev);
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

int prepare_umount(device *d,const char *path){
	mntentry *m;

	if(path == NULL){
		diag("Passed a NULL argument\n");
		return -1;
	}
	if(get_target() == NULL){
		diag("No target is defined\n");
		return -1;
	}
	if(!d->target){
		diag("%s is not mapped into the target\n",d->name);
		return -1;
	}
	if(strcmp(d->target->path,path)){
		diag("%s is mapped to %s, not %s\n",d->name,d->target->path,path);
		return -1;
	}
	m = d->target;
	d->target = NULL;
	if(umount2(path,UMOUNT_NOFOLLOW)){
		diag("Couldn't unmount %s at %s (%s?)\n",
				d->name,d->mnt,strerror(errno));
		// continue anyway...
	}
	free_mntentry(m);
	return 0;
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

// Used to map an on-disk filesystem (which may or may not already be mounted)
// into the target fstab. If already mounted outside the target, the mount is
// preserved in the target definition.
int prepare_mount(device *d,const char *path,const char *cfs,const char *uuid,
				const char *label,const char *ops){
	char devname[PATH_MAX + 1],pathext[PATH_MAX + 1],*fs;
	mntentry *m;

	if(get_target() == NULL){
		diag("No target is defined\n");
		return -1;
	}
	if(d->mnt){
		diag("%s is already actively mounted at %s\n",d->name,d->mnt);
		return -1;
	}
	if(d->target){
		diag("%s is already mapped to %s\n",d->name,d->target->path);
		return -1;
	}
	if(d->swapprio >= SWAP_MAXPRIO){
		diag("%s is used as swap\n",d->name);
		return -1;
	}
	if(snprintf(devname,sizeof(devname),"/dev/%s",d->name) >= (int)sizeof(devname)){
		diag("Bad device name: %s\n",d->name);
		return -1;
	}
	if(snprintf(pathext,sizeof(pathext),"%s/%s",get_target(),path) >= (int)(sizeof(devname) - strlen("/etc/fstab"))){
		diag("Bad mount point: %s\n",path);
		return -1;
	}
	if((fs = strdup(cfs)) == NULL){
		return -1;
	}
	if(targfd < 0){
		if(strcmp(path,"/")){
			diag("Need a root ('/') before mapping %s\n",path);
			free(fs);
			return -1;
		}
		if(mount(devname,pathext,fs,MS_NOATIME,NULL)){
			diag("Couldn't mount %s at %s for %s (%s?)\n",
					devname,pathext,fs,strerror(errno));
			free(fs);
			return -1;
		}
		// we know we have enough space from the check of snprintf()...
		if((targfd = open(pathext,O_DIRECTORY|O_RDONLY|O_CLOEXEC)) < 0){
			diag("Couldn't open %s (%s?)\n",path,strerror(errno));
			free(fs);
			return -1;
		}
		strcat(pathext,"/etc");
		if(mkdir(pathext,0755) && errno != EEXIST){
			diag("Couldn't mkdir %s (%s?)\n",pathext,strerror(errno));
			close(targfd);
			targfd = -1;
			umount2(devname,UMOUNT_NOFOLLOW);
			free(fs);
			return -1;
		}
		d->swapprio = SWAP_INVALID;
		if((d->target = create_target(path,d->name,uuid,label,ops)) == NULL){
			close(targfd);
			targfd = -1;
			free(fs);
			return -1;
		}
		free(d->mnttype);
		d->mnttype = fs;
		return 0;
	}
	if(make_parent_directories(pathext)){
		free(fs);
		return -1;
	}
	// no need to check for preexisting mount at this point -- the mount(2)
	// will fail if one's there.
	if(mount(devname,pathext,fs,MS_NOATIME,NULL)){
		diag("Couldn't mount %s at %s for %s (%s?)\n",devname,pathext,fs,strerror(errno));
		free(fs);
		return -1;
	}
	d->swapprio = SWAP_INVALID;
	if((m = create_target(path,d->name,uuid,label,ops)) == NULL){
		umount2(devname,UMOUNT_NOFOLLOW);
		free(fs);
		return -1;
	}
	free(d->mnttype);
	d->mnttype = fs;
	d->target = m;
	return 0;
}

static const char *
target_path(const char *p,const char *target){
	return p + strlen(target);
}

static int
use_new_target(const char *path){
	const controller *c;

	for(c = get_controllers() ; c ; c = c->next){
		device *d,*p;

		for(d = c->blockdevs ; d ; d = d->next){
			for(p = d->parts ; p ; p = p->next){
				if(p->mnt == NULL){
					continue;
				}
				if(strncmp(path,p->mnt,strlen(path))){
					if((p->target = create_target(target_path(p->mnt,path),p->name,p->uuid,p->label,p->mntops)) == NULL){
						goto err;
					}
				}
			}
			if(d->mnt == NULL){
				continue;
			}
			if(strncmp(path,d->mnt,strlen(path))){
				if((d->target = create_target(target_path(d->mnt,path),d->name,d->uuid,d->label,d->mntops)) == NULL){
					goto err;
				}
			}
		}
	}
	return 0;

err:
	// FIXME strip already-assigned targets
	return -1;
}

int set_target(const char *path){
	const controller *c;

	if(path){
		if(growlight_target){
			diag("A target is already defined: %s\n",growlight_target);
			return -1;
		}
		if(realpath(path,real_target) == NULL){
			diag("Couldn't resolve %s (%s?)\n",path,strerror(errno));
			return -1;
		}
		if(use_new_target(real_target)){
			return -1;
		}
		growlight_target = real_target;
		return 0;
	}
	if(!growlight_target){
		diag("No target is defined\n");
		return -1;
	}
	for(c = get_controllers() ; c ; c = c->next){
		char buf[PATH_MAX + 1];
		device *d,*p;

		for(d = c->blockdevs ; d ; d = d->next){
			if(d->target){
				if(snprintf(buf,sizeof(buf),"%s/%s",growlight_target,d->target->path) >= (int)sizeof(buf)){
					diag("Path too long: %s/%s\n",growlight_target,d->target->path);
				}else if(umount2(buf,UMOUNT_NOFOLLOW)){
					diag("Couldn't unmount %s (%s?)\n",buf,strerror(errno));
				}
				free_mntentry(d->target);
				d->target = NULL;
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->target){
					if(snprintf(buf,sizeof(buf),"%s/%s",growlight_target,d->target->path) >= (int)sizeof(buf)){
						diag("Path too long: %s/%s\n",growlight_target,d->target->path);
					}else if(umount2(buf,UMOUNT_NOFOLLOW)){
						diag("Couldn't unmount %s (%s?)\n",buf,strerror(errno));
					}
					free_mntentry(d->target);
					d->target = NULL;
				}
			}
		}
	}
	growlight_target = NULL;
	close(targfd);
	targfd = -1;
	return 0;
}

int finalize_target(void){
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
	if(vspopen_drain("openvt -s -w -- udpkg --configure --force-configure bootstrap-base")){
		diag("Error installing the base system");
		return -1;
	}
	finalized = 1;
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

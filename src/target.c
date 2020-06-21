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

static char *
dump_controller_modules(void){
	const controller *c;
	char *out,*tmp;
	size_t off;

	off = 0;
	if((out = malloc(sizeof(*out) * (off + 1))) == NULL){
		return NULL;
	}
	out[0] = '\0';
	if(targfd < 0){
		return out;
	}
	for(c = get_controllers() ; c ; c = c->next){
		if(strstr(out,c->driver)){
			continue;
		}
		if((tmp = realloc(out,sizeof(*out) * (strlen(c->driver) + off + 1))) == NULL){
			goto err;
		}
		out = tmp;
		snprintf(out + off,strlen(c->driver) + 1,"%s\n",c->driver);
		off += strlen(c->driver) + 1;
	}
	return out;

err:
	free(out);
	return NULL;
}

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

const char TARGETINFO[] = "Growlight will attempt to prepare a bootable system "
"at %s. At minimum, a target root filesystem must be mounted at this location, "
"so that /etc/fstab and other files can be prepared. The target root must not "
"be mounted with any of the ro, nodev, noexec or nosuid options. "
"You must target a root filesystem first; "
"having done so, you can set up other targets underneath it. Some typical "
"subtargets, none of them required, include /usr/local (so that it can be "
"preserved across installs/machines), /home (for the same reason, and so that "
"nosuid/nodev and/or encryption can be applied), and /var (to protect against "
"its arbitrary growth). Any variety of filesystem can be targeted, but "
"bootloaders typically have their own requirements. Some BIOS firmwares will "
"not attempt to boot from a hard drive lacking an MSDOS partition table, or a "
"partition marked with the bootable flag. UEFI requires a GPT table and an "
"EFI System Partition. Once your targets are configured, finalize the "
"appropriate configuration (one of UEFI, BIOS, or no firmware). Any swap that "
"is enabled will be configured for use on the target machine.";

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
		get_glightui()->boxinfo(TARGETINFO,real_target);
		use_new_target(real_target);
		growlight_target = real_target;
		return 0;
	}
	if(!growlight_target){
		diag("No target is defined\n");
		return -1;
	}
	growlight_target = NULL;
	unmount_target();
	return 0;
}

int finalize_target(void){
	char pathext[PATH_MAX + 1],*fstab,*ftargs;
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
	if((unsigned)snprintf(pathext,sizeof(pathext),"%s/etc/initramfs-tools",growlight_target) >= sizeof(pathext)){
		diag("Name too long (%s/etc/initramfs-tools)\n",growlight_target);
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
	if((fd = openat(targfd,"etc/initramfs-tools/modules",
				O_WRONLY|O_CLOEXEC|O_CREAT,
				S_IROTH | S_IRGRP | S_IWGRP | S_IRUSR | S_IWUSR)) < 0){
		diag("Couldn't open etc/initramfs-tools/modules in target root (%s?)\n",strerror(errno));
		return -1;
	}
	if((fp = fdopen(fd,"w")) == NULL){
		diag("Couldn't get FILE * from %d (%s?)\n",fd,strerror(errno));
		close(fd);
		return -1;
	}
	if((ftargs = dump_controller_modules()) == NULL){
		diag("Couldn't write targets to %s/etc/initramfs-tools/modules (%s?)\n",growlight_target,strerror(errno));
		close(fd);
		return -1;
	}
	if((r = fprintf(fp,"%s",ftargs)) < 0 || (size_t)r < strlen(ftargs)){
		diag("Couldn't write data to %s/etc/initramfs-tools/modules (%s?)\n",growlight_target,strerror(errno));
		free(ftargs);
		close(fd);
		return -1;
	}
	free(ftargs);
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
	if(!d->mnttype){
		return s;
	}
	if(strcmp(d->mnttype,"zfs") == 0){
		return s;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		const char *mnt;
		int dump;

		// Don't write mounts external to the target
		if(strncmp(d->mnt.list[z],growlight_target,strlen(growlight_target))){
			continue;
		}
		if(strcmp(d->mnt.list[z],growlight_target) == 0){
			mnt = "/";
			dump = 1;
		}else{
			mnt = d->mnt.list[z] + strlen(growlight_target);
			dump = 2;
		}
		r = snprintf(NULL,0,"/dev/%s %s %s %s 0 %u\n",fstab_name(d),
				mnt,d->mnttype,d->mntops.list[z],dump);
		if((tmp = realloc(s,sizeof(*s) * (strlen(s) + r + 1))) == NULL){
			goto err;
		}
		s = tmp;
		sprintf(s + strlen(s),"/dev/%s %s %s %s 0 %u\n",fstab_name(d),
				mnt,d->mnttype,d->mntops.list[z],dump);
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
	const controller *c;
	char *out,*tmp;
	size_t off;
	int z;

	off = 0;
	if((out = malloc(sizeof(*out) * (off + 1))) == NULL){
		return NULL;
	}
	out[0] = '\0';
	if(targfd < 0){
		return out;
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

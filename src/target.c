#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>

#include "target.h"
#include "growlight.h"

// Path on the guest filesystem which will hold the target's root filesystem.
char *real_target; // Only used when we set or unset the target
const char *growlight_target = NULL;

static int targfd = -1; // reference to target root, once defined

// Topologically sorted
static struct target {
	mntentry m;
	int fd;			// Reference to mounted directory
	struct target *next;
} *targets;

static struct target *
create_target(const char *path,const char *dev,const char *fs,const char *ops){
	struct target *t;

	if( (t = malloc(sizeof(*t))) ){
		t->m.label = t->m.uuid = NULL;
		t->m.path = strdup(path);
		t->m.dev = strdup(dev);
		t->m.fs = strdup(fs);
		t->m.ops = strdup(ops);
		t->next = NULL;
	}else{
		fprintf(stderr,"Failure creating %s on %s\n",fs,dev);
	}
	return t;
}

static void
free_target(struct target *t){
	if(t){
		free(t->m.label);
		free(t->m.uuid);
		free(t->m.path);
		free(t->m.dev);
		free(t->m.fs);
		free(t->m.ops);
		free(t);
	}
}

void free_targets(void){
	while(targets){
		struct target *t = targets->next;
		free_target(targets);
		targets = t;
	}
}

int prepare_mount(device *d,const char *path,const char *fs,const char *ops){
	struct target **pre,*m;

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
	if(targets == NULL){
		if(strcmp(path,"/")){
			fprintf(stderr,"Need a root ('/') before mapping %s\n",path);
			return -1;
		}
		d->swapprio = SWAP_INVALID;
		free(d->mnttype);
		d->mnttype = NULL;
		if((targets = create_target(path,d->name,fs,ops)) == NULL){
			return -1;
		}
		d->target = &targets->m;
		return 0;
	}
	for(pre = &targets ; *pre ; pre = &(*pre)->next){
		int s;

		if((s = strcmp((*pre)->m.path,path)) == 0){
			fprintf(stderr,"Already have %s at %s\n",(*pre)->m.dev,path);
			return -1;
		}else if(s < 0){
			break;
		}
	}
	d->swapprio = SWAP_INVALID;
	free(d->mnttype);
	d->mnttype = NULL;
	if((m = create_target(path,d->name,fs,ops)) == NULL){
		return -1;
	}
	// FIXME need to actually mount it, no?
	d->target = &m->m;
	m->next = *pre;
	*pre = m;
	return 0;
}

static int
recursive_unmount(struct target **t,char *buf,int n){
	if(*t){
		if(recursive_unmount(&(*t)->next,buf,n)){
			return -1;
		}
		if(snprintf(buf,n,"%s/%s",growlight_target,(*t)->m.path) >= n){
			fprintf(stderr,"Path too long: %s/%s\n",growlight_target,(*t)->m.path);
			return -1;
		}
		if(umount2(buf,UMOUNT_NOFOLLOW)){
			fprintf(stderr,"Couldn't unmount %s (%s?)\n",buf,strerror(errno));
			return -1;
		}
		printf("Unmounted %s at %s\n",(*t)->m.dev,buf);
		*t = NULL;
	}
	return 0;
}

int set_target(const char *path){
	if(path){
		if(growlight_target){
			fprintf(stderr,"A target is already defined: %s\n",growlight_target);
			return -1;
		}
		if((targfd = open(path,O_DIRECTORY|O_RDONLY|O_CLOEXEC)) < 0){
			fprintf(stderr,"Couldn't open %s (%s?)\n",path,strerror(errno));
			return -1;
		}
		if((growlight_target = real_target = strdup(path)) == NULL){
			fprintf(stderr,"Couldn't set target (%s?)\n",strerror(errno));
			close(targfd);
			targfd = -1;
			return -1;
		}
	}else if(growlight_target){
		char buf[PATH_MAX + 1];

		if(recursive_unmount(&targets,buf,sizeof(buf))){
			return -1;
		}
		free(real_target);
		growlight_target = real_target = NULL;
		close(targfd);
		targfd = -1;
	}else{
		fprintf(stderr,"No target is defined\n");
		return -1;
	}
	return 0;
}

int finalize_target(void){
	FILE *fp;
	int fd;

	if(!growlight_target){
		fprintf(stderr,"No target is defined\n");
		return -1;
	}
	if(!targets){
		fprintf(stderr,"No target mappings are defined\n");
		return -1;
	}
	if((fd = openat(targfd,"etc/fstab",O_WRONLY|O_CLOEXEC)) < 0){
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
	const struct target *target = targets;

	if(!target){
		return 0;
	}
	// FIXME allow various naming schemes
	fprintf(fp,"/dev/%s\t%s\t\t%s\t%s\t0\t1\n",target->m.dev,
			target->m.path,target->m.fs,target->m.ops);
	fprintf(fp,"proc\t\t/proc\t\tproc\tdefaults\t0\t0\n");
	while( (target = target->next) ){
		fprintf(fp,"/dev/%s\t%s\t\t%s\t%s\t0\t0\n",
				target->m.dev,target->m.path,target->m.fs,target->m.ops);
	}
	return 0;
}

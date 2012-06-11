#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <target.h>
#include <growlight.h>

// Topologically sorted
static struct target {
	mount m;
	struct target *next;
} *targets;

static struct target *
create_target(const char *path,const char *dev,const char *fs,const char *ops){
	struct target *t;

	if( (t = malloc(sizeof(*t))) ){
		t->m.path = strdup(path);
		t->m.dev = strdup(dev);
		t->m.fs = strdup(fs);
		t->m.ops = strdup(ops);
		t->next = NULL;
	}
	return t;
}

static void
free_target(struct target *t){
	if(t){
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

	if(d->mnt){
		fprintf(stderr,"%s is already actively mounted at %s\n",d->name,d->mnt);
		return -1;
	}
	if(d->target){
		fprintf(stderr,"%s is already mapped to %s\n",d->name,d->target->path);
		return -1;
	}
	if(d->swapprio){
		fprintf(stderr,"%s is used as swap\n",d->name);
		return -1;
	}
	if(targets == NULL){
		if(strcmp(path,"/")){
			fprintf(stderr,"Need a root ('/') before mapping %s\n",path);
			return -1;
		}
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
	if((m = create_target(path,d->name,fs,ops)) == NULL){
		return -1;
	}
	d->target = &m->m;
	m->next = *pre;
	*pre = m;
	return 0;
}

#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <atasmart.h>

#include "smart.h"
#include "growlight.h"

int probe_smart(device *d){
	char path[PATH_MAX];
	SkDisk *sk;

	if(snprintf(path,sizeof(path),"/dev/%s",d->name) >= (int)sizeof(path)){
		fprintf(stderr,"Bad name: %s\n",d->name);
		return -1;
	}
	sk = NULL;
	if(sk_disk_open(path,&sk)){
		verbf("Couldn't probe %s SMART\n",path);
		return -1;
	}
	d->blkdev.smart = 1;
	sk_disk_free(sk);
	return 0;
}

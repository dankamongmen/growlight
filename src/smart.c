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
	uint64_t kelvin;
	SkBool avail;
	SkDisk *sk;

	if(d->layout != LAYOUT_NONE){
		diag("SMART is only available on block devices\n");
		return -1;
	}
	if(snprintf(path,sizeof(path),"/dev/%s",d->name) >= (int)sizeof(path)){
		diag("Bad name: %s\n",d->name);
		return -1;
	}
	sk = NULL;
	if(sk_disk_open(path,&sk)){
		verbf("Couldn't probe %s SMART\n",d->name);
		return -1;
	}
	if(sk_disk_smart_is_available(sk,&avail)){
		verbf("Couldn't probe %s SMART availability\n",path);
		return -1;
	}
	if(!avail){
		sk_disk_free(sk);
		return 0;
	}
	if(sk_disk_smart_read_data(sk)){
		verbf("Couldn't read %s SMART data\n",d->name);
		return -1;
	}
	if(avail){
		d->blkdev.smart = 1;
	}else{
		verbf("SMART is unavailable: %s\n",d->name);
	}
	if(sk_disk_smart_get_temperature(sk,&kelvin) == 0){
		verbf("Disk (%s) temperature: %ju\n",d->name,(uintmax_t)kelvin);
		d->blkdev.kelvin = kelvin;
	}
	sk_disk_free(sk);
	return 0;
}

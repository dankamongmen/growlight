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
	SkBool avail,good;
	uint64_t kelvin;
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
		verbf("SMART is unavailable: %s\n",d->name);
		sk_disk_free(sk);
		return 0;
	}
	if(sk_disk_smart_read_data(sk)){
		verbf("Couldn't read %s SMART data\n",d->name);
		return -1;
	}
	if(sk_disk_smart_status(sk,&good) == 0){
		verbf("Disk (%s) SMART status: %s\n",d->name,good ? "Good" : "Bad");
		if(good){
			d->blkdev.smartgood = SMART_STATUS_GOOD;
		}else{
			d->blkdev.smartgood = SMART_STATUS_BAD;
		}
	}else{
		d->blkdev.smartgood = SMART_NOSUPPORT;
	}
	if(sk_disk_smart_get_temperature(sk,&kelvin) == 0){
		d->blkdev.celsius = (kelvin - 273150) / 1000;
		verbf("Disk (%s) temperature: %ju\n",d->name,d->blkdev.celsius);
	}
	sk_disk_free(sk);
	return 0;
}

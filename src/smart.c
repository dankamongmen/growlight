// copyright 2012–2021 nick black
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
	SkBool avail, good;
	uint64_t kelvin;
	SkDisk *sk;

	if(d->layout != LAYOUT_NONE){
		diag("SMART is only available on block devices\n");
		return -1;
	}
	d->blkdev.smart = -1;
	if(snprintf(path, sizeof(path), "/dev/%s", d->name) >= (int)sizeof(path)){
		diag("Bad name: %s\n", d->name);
		return -1;
	}
	sk = NULL;
	if(sk_disk_open(path, &sk)){
		verbf("Couldn't probe %s SMART\n", d->name);
		return -1;
	}
	if(sk_disk_smart_is_available(sk, &avail)){
		verbf("Couldn't probe %s SMART availability\n", path);
		sk_disk_free(sk);
		return -1;
	}
	if(!avail){
		verbf("SMART is unavailable: %s\n", d->name);
		sk_disk_free(sk);
		return 0;
	}
	if(sk_disk_smart_read_data(sk)){
		verbf("Couldn't read %s SMART data\n", d->name);
		sk_disk_free(sk);
		return -1;
	}
	if(sk_disk_smart_status(sk, &good) == 0){
		SkSmartOverall overall;

		if(sk_disk_smart_get_overall(sk, &overall)){
			if(good){
				d->blkdev.smart = SK_SMART_OVERALL_GOOD;
			}else{
				d->blkdev.smart = SK_SMART_OVERALL_BAD_STATUS;
			}
		}else{
			d->blkdev.smart = overall;
		}
		verbf("Disk (%s) SMART status: %d\n", d->name, d->blkdev.smart);
	}
	if(sk_disk_smart_get_temperature(sk, &kelvin) == 0){
		d->blkdev.celsius = (kelvin - 273150) / 1000;
		verbf("Disk (%s) temperature: %ju\n", d->name, d->blkdev.celsius);
	}
	sk_disk_free(sk);
	return 0;
}

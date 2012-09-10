#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "popen.h"
#include "health.h"
#include "growlight.h"

int badblock_scan(device *d,unsigned rw){
	char cmd[PATH_MAX];

	if(d->layout != LAYOUT_NONE){
		diag("Block scans are performed only on raw block devices\n");
		return -1;
	}
	// FIXME supply -b blocksize argument!
	if(snprintf(cmd,sizeof(cmd),"badblocks -v -s %s /dev/%s",
		rw ? d->mnt.count ? "-n" : "-w" : "",d->name) >= (int)sizeof(cmd)){
		diag("Bad name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(cmd)){
		return -1;
	}
	return 0;
}

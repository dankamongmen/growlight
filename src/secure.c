#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "popen.h"
#include "secure.h"
#include "growlight.h"

int ata_secure_erase(device *d){
	char buf[PATH_MAX];

	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Can only run ATA Erase on ATA-connected blockdevs\n");
		return -1;
	}
	if(snprintf(buf,sizeof(buf),"/sbin/hdparm --user-master u --security-set-pass erasepw /dev/%s",d->name) >= (int)sizeof(buf)){
		fprintf(stderr,"Bad pw name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(buf)){
		fprintf(stderr,"Couldn't set ATA user password\n");
		return -1;
	}
	if(snprintf(buf,sizeof(buf),"/sbin/hdparm --user-master u --security-erase erasepw /dev/%s",d->name) >= (int)sizeof(buf)){
		fprintf(stderr,"Bad erase name: %s\n",d->name);
		return -1;
	}
	if(popen_drain(buf)){
		fprintf(stderr,"Couldn't perform ATA Secure Erase\n");
		return -1;
	}
	return 0;
}

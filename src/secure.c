#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "secure.h"
#include "growlight.h"

int ata_secure_erase(device *d){
	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Can only run ATA Erase on ATA-connected blockdevs\n");
		return -1;
	}
	fprintf(stderr,"FIXME not yet implemented!\n");
	return 0;
}

// copyright 2012–2021 nick black
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "popen.h"
#include "secure.h"
#include "growlight.h"

int ata_secure_erase(device *d){
	if(d->layout != LAYOUT_NONE){
		diag("Can only run ATA Erase on ATA-connected blockdevs\n");
		return -1;
	}
	if(vspopen_drain("hdparm --user-master u --security-set-pass erasepw /dev/%s", d->name)){
		diag("Couldn't set ATA user password\n");
		return -1;
	}
	if(vspopen_drain("hdparm --user-master u --security-erase erasepw /dev/%s", d->name)){
		diag("Couldn't perform ATA Secure Erase\n");
		return -1;
	}
	return 0;
}

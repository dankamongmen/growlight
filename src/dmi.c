// copyright 2012–2020 nick black
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "sysfs.h"
#include "growlight.h"

#define DMI_PATH "/sys/devices/virtual/dmi/id"

static char *bios_vendor;
static char *bios_version;

int dmi_init(void){
	int dfd;

	if((dfd = open(DMI_PATH, O_RDONLY|O_DIRECTORY)) < 0){
		diag("Couldn't open %s (%s)\n", DMI_PATH, strerror(errno));
		return -1;
	}
	if((bios_version = get_sysfs_string(dfd, "bios_version")) == NULL){
		diag("Couldn't open %s/%s (%s)\n", DMI_PATH, "bios_version", strerror(errno));
	}
	if((bios_vendor = get_sysfs_string(dfd, "bios_vendor")) == NULL){
		diag("Couldn't open %s/%s (%s)\n", DMI_PATH, "bios_vendor", strerror(errno));
	}
	return 0;
}

const char *get_bios_version(void){
	return bios_version;
}

const char *get_bios_vendor(void){
	return bios_vendor;
}

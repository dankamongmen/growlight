#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "zfs.h"
#include "config.h"
#include "growlight.h"

#ifdef HAVE_LIBZFS

// FIXME hacks around the libspl/libzfs autotools-dominated jank
#define HAVE_IOCTL_IN_SYS_IOCTL_H
#define ulong_t unsigned long
#define boolean_t bool
#include <stdbool.h>
#include <libzfs.h>

static libzfs_handle_t *zht;

int init_zfs_support(void){
	if((zht = libzfs_init()) == NULL){
		return -1;
	}
	verbf("Initialized ZFS support.\n");
	return 0;
}

int stop_zfs_support(void){
	libzfs_fini(zht);
	zht = NULL;
	return 0;
}
#else
int init_zfs_support(void){
	verbf("No ZFS support in this build.\n");
	return 0;
}

int stop_zfs_support(void){
	return 0;
}
#endif

// copyright 2012–2020 nick black
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>

#include "swap.h"
#include "popen.h"
#include "growlight.h"

int mkswap(device *d){
	if(d->mnttype && strcmp(d->mnttype, "swap")){
		diag("Won't create swap on %s filesystem at %s\n",
				d->mnttype, d->name);
		return -1;
	}
	if(d->swapprio >= SWAP_MAXPRIO){
		diag("Already swapping on %s\n", d->name);
		return -1;
	}
	if(vspopen_drain("mkswap -L SprezzaSwap /dev/%s", d->name)){
		return -1;
	}
	return 0;
}

// Create swap on the device, and use it
int swapondev(device *d){
	char fn[PATH_MAX], *mt;

	if(mkswap(d)){
		return -1;
	}
	snprintf(fn, sizeof(fn), "/dev/%s", d->name);
	if((mt = strdup("swap")) == NULL){
		return -1;
	}
	if(swapon(fn, 0)){
		diag("Couldn't swap on %s (%s?)\n", fn, strerror(errno));
		free(mt);
		return -1;
	}
	free(d->mnttype);
	d->mnttype = mt;
	d->swapprio = SWAP_MAXPRIO; // FIXME take as param
	return 0;
}

// Deactive the swap on this partition (if applicable)
int swapoffdev(device *d){
	char fn[PATH_MAX];

	snprintf(fn, sizeof(fn), "/dev/%s", d->name);
	if(swapoff(fn)){
		diag("Couldn't stop swapping on %s (%s?)\n", fn, strerror(errno));
		return -1;
	}
	d->swapprio = SWAP_INACTIVE;
	return 0;
}

// Parse /proc/swaps to detect active swap devices
int parse_swaps(const glightui *gui, const char *name){
	char buf[BUFSIZ];
	int line = 0;
	FILE *fp;

	if((fp = fopen(name, "re")) == NULL){
		diag("Couldn't open %s (%s?)\n", name, strerror(errno));
		return -1;
	}
	// First line is a legend
	while(fgets(buf, sizeof(buf), fp)){
		char *toke = buf, *type, *size, *e;
		device *d;

		if(++line == 1){
			continue;
		}
		while(isgraph(*toke)){ // First field: "Filename"
			++toke;
		}
		*toke++ = '\0';
		while(isspace(*toke)){
			++toke;
		}
		type = toke++;		// Second field: "Type". "block"/"file"
		while(isgraph(*toke)){
			++toke;
		}
		*toke++ = '\0';
		if(strcmp(type, "file") == 0){
			continue;
		}
		if((d = lookup_device(buf)) == NULL){
			goto err;
		}
		size = toke++;		// Third field: "Size".
		while(isgraph(*toke)){
			++toke;
		}
		*toke++ = '\0';
		errno = 0;
		if(((d->mntsize = strtoull(size, &e, 0)) == ULLONG_MAX && errno == ERANGE) ||
				d->mntsize == 0 || *e){
			goto err;
		}
		d->mntsize *= 1024;
		if(d->swapprio == SWAP_INVALID){
			if(!d->mnttype || strcmp(d->mnttype, "swap")){
				if(d->mnttype){
					diag("Warning: %s went from %s to swap\n", d->name, d->mnttype);
					free(d->mnttype);
					d->mnttype = NULL;
				}
				if((d->mnttype = strdup("swap")) == NULL){
					goto err;
				}
			}
			// FIXME we can get the real priority from the last field
			d->swapprio = SWAP_MAXPRIO; // FIXME
			if(d->layout == LAYOUT_PARTITION){
				d = d->partdev.parent;
			}
			d->uistate = gui->block_event(d, d->uistate);
		}
	}
	fclose(fp);
	return 0;

err:
	fclose(fp);
	return -1;
}

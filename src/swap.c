#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/swap.h>

#include <swap.h>
#include <growlight.h>

// Create swap on the device, and use it
int swapondev(device *d){
	char fn[PATH_MAX];

	snprintf(fn,sizeof(fn),"/dev/%s",d->name);
	if(d->target){
		fprintf(stderr,"Won't create swap on target mount %s (%s)\n",
				d->name,d->target->path);
		return -1;
	}
	if(d->mnt){
		fprintf(stderr,"Won't create swap on active mount %s (%s)\n",
				d->name,d->mnt);
		return -1;
	}
	if(swapon(fn,0)){
		fprintf(stderr,"Couldn't swap on %s (%s?)\n",fn,strerror(errno));
		return -1;
	}
	return 0;
}

// Deactive the swap on this partition (if applicable)
int swapoffdev(device *d){
	char fn[PATH_MAX];

	snprintf(fn,sizeof(fn),"/dev/%s",d->name);
	if(swapoff(fn)){
		fprintf(stderr,"Couldn't stop swapping on %s (%s?)\n",fn,strerror(errno));
		return -1;
	}
	return 0;
}

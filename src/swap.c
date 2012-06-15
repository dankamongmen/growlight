#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>

#include <swap.h>
#include <growlight.h>

int mkswap(device *d){
	char cmd[PATH_MAX],buf[BUFSIZ];
	ssize_t r;
	FILE *fp;

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
	if(d->swapprio >= SWAP_MAXPRIO){
		fprintf(stderr,"Already swapping on %s\n",d->name);
		return -1;
	}
	if(snprintf(cmd,sizeof(cmd),"/sbin/mkswap -L SprezzaSwap /dev/%s",d->name) >= (int)sizeof(cmd)){
		fprintf(stderr,"Error building command line for %s\n",d->name);
		return -1;
	}
	if((fp = popen(cmd,"r")) == NULL){
		fprintf(stderr,"Error running '%s' (%s?)\n",cmd,strerror(errno));
		return -1;
	}
	while((r = fread(buf,1,BUFSIZ,fp)) > 0){
		if(fwrite(buf,1,r,stdout) < (size_t)r){
			fprintf(stderr,"Error echoing '%s' (%s?)\n",cmd,strerror(errno));
			fclose(fp);
			return -1;
		}
	}
	if(r < 0){
		fprintf(stderr,"Error draining '%s' (%s?)\n",cmd,strerror(errno));
		fclose(fp);
		return -1;
	}
	if(fclose(fp)){
		fprintf(stderr,"Error closing '%s' (%s?)\n",cmd,strerror(errno));
		return -1;
	}
	return 0;
}

// Create swap on the device, and use it
int swapondev(device *d){
	char fn[PATH_MAX],*mt;

	if(mkswap(d)){
		return -1;
	}
	snprintf(fn,sizeof(fn),"/dev/%s",d->name);
	if((mt = strdup("swap")) == NULL){
		return -1;
	}
	if(swapon(fn,0)){
		fprintf(stderr,"Couldn't swap on %s (%s?)\n",fn,strerror(errno));
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

	snprintf(fn,sizeof(fn),"/dev/%s",d->name);
	if(swapoff(fn)){
		fprintf(stderr,"Couldn't stop swapping on %s (%s?)\n",fn,strerror(errno));
		return -1;
	}
	d->swapprio = SWAP_INACTIVE;
	return 0;
}

// Parse /proc/swaps to detect active swap devices
int parse_swaps(void){
	char buf[BUFSIZ];
	int line = 0;
	FILE *fp;

	if((fp = fopen("/proc/swaps","r")) == NULL){
		fprintf(stderr,"Couldn't open /proc/swaps (%s?)\n",strerror(errno));
		return -1;
	}
	// First line is a legend
	while(fgets(buf,sizeof(buf),fp)){
		char *toke = buf;
		device *d;

		if(++line == 1){
			continue;
		}
		while(isgraph(*toke)){
			++toke;
		}
		*toke = '\0';
		if((d = lookup_device(buf)) == NULL){
			goto err;
		}
		if(d->mnttype){
			fprintf(stderr,"Warning: %s went from %s to swap\n",d->name,d->mnttype);
			free(d->mnttype);
			// FIXME...
		}
		if((d->mnttype = strdup("swap")) == NULL){
			goto err;
		}
		// FIXME we can get the real priority from the last field
		d->swapprio = SWAP_MAXPRIO; // FIXME
	}
	fclose(fp);
	return 0;

err:
	fclose(fp);
	return -1;
}

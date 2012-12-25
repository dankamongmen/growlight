#include "ssd.h"
#include "popen.h"
#include "growlight.h"

int fstrim(const char *mnt){
	return vspopen_drain("fstrim -v %s",mnt);
}

int fstrim_dev(device *d){
	unsigned z;
	int ret;

	if(!d->mnttype){
		diag("No filesystem on %s\n",d->name);
		return -1;
	}
	if(d->mnt.count){
		diag("%s is in use (%ux) and cannot be wiped\n",d->name,d->mnt.count);
		return -1;
	}
	ret = 0;
	for(z = 0 ; z < d->mnt.count ; ++z){
		ret |= fstrim(d->mnt.list[z]);
	}
	return ret;
}

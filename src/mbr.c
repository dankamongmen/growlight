#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>
#include <sys/mman.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#include "mbr.h"
#include "growlight.h"

#define MBR_SIZE 512
#define MBR_CODE_SIZE 440

int mbrsha1(int fd,void *buf){
	// We only check the first MBR_CODE_SIZE bytes, but must read in
	// multiples of the sector size. This code might in fact be broken
	// for 4k sector disks -- I'm unsure whether it's logical or physical.
	// FIXME find out!
	unsigned char mbr[MBR_SIZE];
	ssize_t r;

	if(lseek(fd,0,SEEK_SET)){
		int e = errno;
		diag("Couldn't seek to first byte of %d (%s?)\n",fd,strerror(errno));
		errno = e;
		return -1;
	}
	if((r = read(fd,mbr,sizeof(mbr))) < 0 || r < (int)sizeof(mbr)){
		diag("Read %zd/%zu of %d (%s?)\n",r,sizeof(mbr),fd,strerror(errno));
		return -1;
	}
	if(SHA1(mbr,MBR_CODE_SIZE,buf) == NULL){
		diag("Couldn't perform SHA1 for %d (%s)\n",fd,ERR_lib_error_string(ERR_get_error()));
		return -1;
	}
	return 0;
}

int zerombrp(const void *buf){
	const void *z = "\x63\x9a\xc5\xcd\xf8\xa5\xcf\x32\x45\x97\x59\x32\xc6\xa4\x21\x54\x50\xa7\xb9\x8f";

	return !memcmp(buf,z,20);
}

static inline int
wipe_first_sector(device *d,size_t wipe,size_t wipeend){
	static char buf[MBR_SIZE];
	char dbuf[PATH_MAX];
	int fd,pgsize;
	void *map;

	if((pgsize = getpagesize()) < 0){
		diag("Couldn't get page size\n");
		return -1;
	}
	if(wipeend > sizeof(buf) || wipe >= wipeend){
		diag("Can't wipe %zu/%zu/%zu\n",wipe,wipeend,sizeof(buf));
		return -1;
	}
	if(d->layout != LAYOUT_NONE){
		diag("Will only wipe BIOS state for block devices\n");
		return -1;
	}
	if(snprintf(dbuf,sizeof(dbuf),"/dev/%s",d->name) >= (int)sizeof(dbuf)){
		diag("Bad device name: %s\n",d->name);
		return -1;
	}
	if((fd = openat(devfd,d->name,O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		int e = errno;
		diag("Couldn't open /dev/%s (%s?)\n",d->name,strerror(errno));
		errno = e;
		return -1;
	}
	map = mmap(NULL,pgsize,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_LOCKED,fd,0);
	if(map == MAP_FAILED){
		int e = errno;
		diag("Couldn't map %d from %d on %s (%s?)\n",pgsize,fd,dbuf,strerror(errno));
		close(fd);
		errno = e;
		return -1;
	}
	memcpy((char *)map + wipe,buf,wipeend - wipe);
	if(munmap(map,pgsize)){
		int e = errno;
		diag("Couldn't unmap %d from %d on %s (%s?)\n",pgsize,fd,dbuf,strerror(errno));
		close(fd);
		errno = e;
		return -1;
	}
	if(mbrsha1(fd,d->blkdev.biossha1)){
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if(close(fd)){
		int e = errno;
		diag("Couldn't close %s (%s?)\n",dbuf,strerror(errno));
		errno = e;
		return -1;
	}
	if(zerombrp(d->blkdev.biossha1)){
		d->blkdev.biosboot = 0;
	}
	// FIXME we still have valid filesystems, but no longer have valid
	//   partition table entries for them (iff we were using MBR). add
	//   "recovery"? gparted can supposedly find lost filesystems....
	if(rescan_blockdev(d)){
		return -1;
	}
	return 0;
}

int wipe_biosboot(device *d){
	return wipe_first_sector(d,0,MBR_CODE_SIZE);
}

int wipe_dosmbr(device *d){
	if(wipe_first_sector(d,0,MBR_SIZE)){
		assert(0);
		return -1;
	}
	return 0;
}

int wipe_dos_ptable(device *d){
	if(wipe_first_sector(d,MBR_CODE_SIZE,MBR_SIZE)){
		return -1;
	}
	return 0;
}

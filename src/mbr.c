#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#include <mbr.h>
#include <growlight.h>

#define MBR_SIZE 512
#define MBR_CODE_SIZE 440

int mbrsha1(int fd,void *buf){
	unsigned char mbr[MBR_CODE_SIZE];
	ssize_t r;

	if(lseek(fd,0,SEEK_SET)){
		int e = errno;
		fprintf(stderr,"Couldn't seek to first byte of %d (%s?)\n",fd,strerror(errno));
		errno = e;
		return -1;
	}
	if((r = read(fd,mbr,sizeof(mbr))) < 0 || r < (int)sizeof(mbr)){
		verbf("Read %zd/%zu of %d (%s?)\n",r,sizeof(mbr),fd,strerror(errno));
		return -1;
	}
	if(SHA1(mbr,sizeof(mbr),buf) == NULL){
		fprintf(stderr,"Couldn't perform SHA1 for %d (%s)\n",fd,ERR_lib_error_string(ERR_get_error()));
		return -1;
	}
	return 0;
}

int zerombrp(const void *buf){
	const void *z = "\x63\x9a\xc5\xcd\xf8\xa5\xcf\x32\x45\x97\x59\x32\xc6\xa4\x21\x54\x50\xa7\xb9\x8f";

	return !memcmp(buf,z,20);
}

static inline int
wipe_first_sector(device *d,size_t wipe){
	static char buf[MBR_SIZE];
	char dbuf[PATH_MAX];
	ssize_t w;
	int fd;

	if(wipe > sizeof(buf)){
		fprintf(stderr,"Can't wipe %zu\n",wipe);
		return -1;
	}
	if(d->layout != LAYOUT_NONE){
		fprintf(stderr,"Will only wipe BIOS state for block devices\n");
		return -1;
	}
	if(snprintf(dbuf,sizeof(dbuf),"/dev/%s",d->name) >= (int)sizeof(dbuf)){
		fprintf(stderr,"Bad device name: %s\n",d->name);
		return -1;
	}
	if((fd = open(dbuf,O_RDWR|O_CLOEXEC)) < 0){
		int e = errno;
		fprintf(stderr,"Couldn't open %s (%s?)\n",dbuf,strerror(errno));
		errno = e;
		return -1;
	}
	if(lseek(fd,0,SEEK_SET)){
		int e = errno;
		fprintf(stderr,"Couldn't seek to first byte of %s (%s?)\n",dbuf,strerror(errno));
		close(fd);
		errno = e;
		return -1;
	}
	if((w = write(fd,buf,wipe)) < 0 || w < (int)wipe){
		int e = errno;
		fprintf(stderr,"Couldn't write to first sector of %s (%s?)\n",dbuf,strerror(errno));
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
		fprintf(stderr,"Couldn't close %s (%s?)\n",dbuf,strerror(errno));
		errno = e;
		return -1;
	}
	if(zerombrp(d->blkdev.biossha1)){
		d->blkdev.biosboot = 0;
	}
	// FIXME we still have valid filesystems, but no longer have valid
	//   partition table entries for them (iff we were using MBR). add
	//   "recovery"? gparted can supposedly find lost filesystems....
	if(reset_blockdev(d)){
		return -1;
	}
	return 0;
}

int wipe_biosboot(device *d){
	return wipe_first_sector(d,MBR_CODE_SIZE);
}

int wipe_dosmbr(device *d){
	if(wipe_first_sector(d,MBR_SIZE)){
		return -1;
	}
	// FIXME remove partition entries
	return 0;
}

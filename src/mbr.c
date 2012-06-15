#include <assert.h>
#include <ctype.h>
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

	lseek(fd,0,SEEK_SET);
	if((r = read(fd,mbr,sizeof(mbr))) < 0){
		return -1;
	}
	if(r < (int)sizeof(mbr)){
		return -1;
	}
	if(SHA1(mbr,sizeof(mbr),buf) == NULL){
		fprintf(stderr,"Couldn't perform SHA1 for %d (%s)\n",fd,ERR_lib_error_string(ERR_get_error()));
	}
	return 0;
}

int zerombrp(const void *buf){
	const void *z = "\x63\x9a\xc5\xcd\xf8\xa5\xcf\x32\x45\x97\x59\x32\xc6\xa4\x21\x54\x50\xa7\xb9\x8f";

	return !memcmp(buf,z,20);
}

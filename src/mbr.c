#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>
#include <openssl/sha.h>

#include <mbr.h>
#include <growlight.h>

#define MBR_SIZE 512
#define MBR_CODE_SIZE 444

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
	SHA1(mbr,sizeof(mbr),buf);
	return 0;
}

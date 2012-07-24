#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <linux/kdev_t.h>

#include "sysfs.h"
#include "growlight.h"

// FIXME use libudev or at least mmap.c for this crap
// FIXME sysfs is UTF-8 not ASCII!
char *get_sysfs_string(int dirfd,const char *node){
	char buf[512]; // FIXME
	ssize_t r;
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return NULL;
	}
	if((r = read(fd,buf,sizeof(buf))) <= 0){
		int e = errno;
		close(fd);
		errno = e;
		return NULL;
	}
	if((size_t)r >= sizeof(buf) || buf[r - 1] != '\n'){
		close(fd);
		errno = ENAMETOOLONG;
		return NULL;
	}
	close(fd);
	buf[r - 1] = '\0';
	return strdup(buf);
}

int sysfs_devno(int dirfd,dev_t *devno){
	int fd = openat(dirfd,"dev",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
	const char *colon;
	char buf[512]; // FIXME
	ssize_t r;

	if(fd < 0){
		return -1;
	}
	if((r = read(fd,buf,sizeof(buf))) <= 0){
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if((size_t)r >= sizeof(buf) || buf[r - 1] != '\n'){
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	close(fd);
	if((colon = strchr(buf,':')) == NULL){
		return -1;
	}
	*devno = MKDEV(atoi(buf),atoi(colon + 1));
	return 0;
}

unsigned sysfs_exist_p(int dirfd,const char *node){
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return 0;
	}
	close(fd);
	return 1;
}

int get_sysfs_bool(int dirfd,const char *node,unsigned *b){
	char buf[512]; // FIXME
	ssize_t r;
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return -1;
	}
	if((r = read(fd,buf,sizeof(buf))) <= 0){
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if((size_t)r >= sizeof(buf) || buf[r - 1] != '\n'){
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	close(fd);
	buf[r - 1] = '\0';
	*b = strcmp(buf,"0") ? 1 : 0;
	return 0;
}

int get_sysfs_uint(int dirfd,const char *node,unsigned long *b){
	char *end,buf[512]; // FIXME
	ssize_t r;
	int fd;

	if((fd = openat(dirfd,node,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return -1;
	}
	if((r = read(fd,buf,sizeof(buf))) <= 0){
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if((size_t)r >= sizeof(buf) || buf[r - 1] != '\n'){
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	close(fd);
	buf[r - 1] = '\0';
	*b = strtoul(buf,&end,0);
	if(*end){
		diag("Malformed sysfs uint: %s\n",buf);
		return -1;
	}
	return 0;
}

int write_sysfs(const char *name,const char *str){
	ssize_t w;
	int fd;

	if((fd = open(name,O_WRONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		return -1;
	}
	if((w = write(fd,str,strlen(str))) <= 0 || w < (int)strlen(str)){
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	close(fd);
	return 0;
}

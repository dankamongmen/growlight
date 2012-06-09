#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <mmap.h>

// Handle files which aren't easily supported by mmap(), such as /proc entries
// which don't return their true lengths to fstat() and friends. fd should be
// positioned at the beginning of the file. We use anonymous mappings rather
// than malloc() so that we can pass everything back to munmap().
static void *
read_map_virt_fd(int fd,off_t *len){
	int pgsize = getpagesize();
	void *map = MAP_FAILED;
	char buf[pgsize];
	ssize_t r;
	*len = 0;

	if(pgsize < 0){
		fprintf(stderr,"Invalid pagesize: %d (%s?)\n",pgsize,strerror(errno));
		return MAP_FAILED;
	}
	while((r = read(fd,buf,sizeof(buf))) > 0){
		// FIXME handle loops!
		map = mmap(NULL,*len + r,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
		if(map == MAP_FAILED){
			goto maperr;
		}
		memcpy((char *)map + *len,buf,r);
		*len += r;
	}
	if(r < 0){
		int e = errno;
		fprintf(stderr,"Error reading %d (%s?)\n",fd,strerror(errno));
		if(map != MAP_FAILED){
			munmap(map,*len);
		}
		errno = e;
		return MAP_FAILED;
	}
	if(*len == 0){
		*len = pgsize;
		map = mmap(NULL,*len,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
		if(map == MAP_FAILED){
			goto maperr;
		}
	}
	return map;

maperr:
	fprintf(stderr,"Couldn't extend map of %d past %ju (%s?)\n", fd,*len,strerror(errno));
	*len = -1;
	return MAP_FAILED;
}

void *map_virt_fd(int fd,off_t *len){
	struct stat st;
	void *map;

	if(fstat(fd,&st)){
		fprintf(stderr,"Couldn't get size of fd %d (%s?)\n",fd,strerror(errno));
		return MAP_FAILED;
	}
	// Most /proc entries return a 0 length
	if((*len = st.st_size) == 0){
		return read_map_virt_fd(fd,len);
	}
	if((map = mmap(NULL,*len,PROT_READ,MAP_SHARED,fd,0)) == MAP_FAILED){
		int e = errno;
		fprintf(stderr,"Couldn't map %ju at %d (%s?)\n",
				(uintmax_t)*len,fd,strerror(errno));
		errno = e;
		return MAP_FAILED;
	}
	return map;
}

void *map_virt_file(const char *fn,int *fd,off_t *len){
	void *map;
	int tfd;

	if((tfd = open(fn,O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
		int e = errno;
		fprintf(stderr,"Couldn't open %s (%s?)\n",fn,strerror(errno));
		errno = e;
		return MAP_FAILED;
	}
	if((map = map_virt_fd(tfd,len)) == MAP_FAILED){
		close(tfd);
		return MAP_FAILED;
	}
	*fd = tfd;
	return map;
}


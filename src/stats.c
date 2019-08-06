#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include "stats.h"
#include <unistd.h>
#include <stdlib.h>
#include "growlight.h"

const char PROCFS_DISKSTATS[] = "/proc/diskstats";

int read_proc_diskstats(diskstats *prev, int prevcount, diskstats **stats) {
	return read_diskstats(PROCFS_DISKSTATS, prev, prevcount, stats);
}

// procfs files can't be mmap()ed, and always advertise a length of 0. They are
// to be open()ed, read() in one go, and close()d. Returns a heap-allocated
// image of the procfs file, setting *buflen to the number of bytes read. An
// existing, empty file will result in a non-NULL return and a *buflen of 0. Any
// error will result in a return of NULL and an undefined *buflen.
//
// Technically, this function will work on any file supporting read(), but usual
// disk files are typically better mmap()ped.
static char *
read_procfs_file(const char *path, size_t *buflen) {
	size_t alloclen = 0;
	char *buf = NULL;
	int fd, terrno;
	ssize_t r;

	if((fd = open(path, O_CLOEXEC|O_RDONLY)) < 0){
		return NULL;
	}
	*buflen = 0;
	r = 0;
	do{
		*buflen += r;
		if(*buflen == alloclen){
			char *tmp = realloc(buf, alloclen + BUFSIZ);
			if(tmp == NULL){
				terrno = errno;
				free(buf);
				close(fd);
				errno = terrno;
				return NULL;
			}
			buf = tmp;
			alloclen += BUFSIZ;
		}
	}while((r = read(fd, buf + *buflen, alloclen - *buflen)) > 0);
	if(r < 0){
		terrno = errno;
		diag("Error reading %zu from %s (%s)\n", *buflen, path, strerror(terrno));
		free(buf);
		close(fd);
		errno = terrno;
		return NULL;
	}
	diag("Read %zub from %s", *buflen, path);
	close(fd);
	return buf;
}

int read_diskstats(const char *path, diskstats *prev, int prevcount, diskstats **stats) {
	size_t buflen;
	char *buf;

	if((buf = read_procfs_file(path, &buflen)) == NULL){
		return -1;
	}
	free(buf);
	return 0;
}

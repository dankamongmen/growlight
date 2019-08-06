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

// Find the offset one past the end of the current line (presumed to start at
// offset). Offset must be less than or equal to buflen. The returned value
// will never be greater than buflen.
static size_t
find_line_end(const char *buf, size_t offset, size_t buflen) {
	while(offset < buflen){
		if(buf[offset] == '\n'){
			++offset;
			break;
		}
	}
	return offset;
}

static int
add_diskstat(diskstats *prev, int prevcount, diskstats **stats, int devcount) {
	diskstats dstat;
	memset(&dstat, 0, sizeof(dstat));
	// FIXME lex line, expand stats
	diskstats *tmp = realloc(*stats, sizeof(**stats) * (devcount + 1));
	if(tmp == NULL){
		return -1;
	}
	memcpy(&tmp[devcount], &dstat, sizeof(dstat));
	*stats = tmp;
	return 0;
}

int read_diskstats(const char *path, diskstats *prev, int prevcount,
		   diskstats **stats) {
	size_t buflen;
	char *buf;

	if((buf = read_procfs_file(path, &buflen)) == NULL){
		return -1;
	}
	// FIXME sort the input for quicker delta generation?
	size_t offset = 0; // where our line starts in the file
	size_t eol; // points one past last byte of line after find_line_end()
	int devices = 0;
	*stats = NULL;
	while((eol = find_line_end(buf, offset, buflen)) > offset){
		// FIXME do someting with prev/prevcount
		if(add_diskstat(prev, prevcount, stats, devices)){
			free(*stats);
			free(buf);
			return -1;
		}
		++devices;
		offset = eol + 1;
	}
	free(buf);
	return devices;
}

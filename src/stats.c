#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include "stats.h"
#include <unistd.h>
#include <stdlib.h>
#include "growlight.h"

const char PROCFS_DISKSTATS[] = "/proc/diskstats";

int read_proc_diskstats(diskstats **stats) {
	return read_diskstats(PROCFS_DISKSTATS, stats);
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
// diag("Read %zub from %s\n", *buflen, path);
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
		++offset;
	}
	return offset;
}

static int
add_diskstat(diskstats **stats, int devcount, diskstats *dstat) {
	diskstats *tmp = realloc(*stats, sizeof(**stats) * (devcount + 1));
	if(tmp == NULL){
		return -1;
	}
	memcpy(&tmp[devcount], dstat, sizeof(*dstat));
	*stats = tmp;
	return 0;
}

// Lex the major/minor number and device name. Copy the device name into dstat.
// Returns the number of characters consumed, or -1 on a lexing failure.
static int
lex_diskstats_prefix(const char *sol, const char *eol, diskstats *dstat) {
	const char *start = sol;
	// Pass any initial whitespace
	while(sol < eol && isspace(*sol)){
		++sol;
	}
	// Pass major number
	while(sol < eol && isdigit(*sol)){
		++sol;
	}
	// Should have whitespace now
	if(sol == eol || !isspace(*sol)){
		return -1;
	}
	// Pass said whitespace
	do{
		++sol;
	}while(sol < eol && isspace(*sol));
	// Pass minor number
	while(sol < eol && isdigit(*sol)){
		++sol;
	}
	// Should have whitespace now
	if(sol == eol || !isspace(*sol)){
		return -1;
	}
	// Pass said whitespace
	do{
		++sol;
	}while(sol < eol && isspace(*sol));
	unsigned namelen = 0;
	while(sol < eol && isgraph(*sol)){
		dstat->name[namelen++] = *sol;
		++sol;
		if(namelen >= sizeof(dstat->name)){ // name was too long, aieee
			return -1;
		}
	}
	if(namelen == 0){
		return -1;
	}
	dstat->name[namelen] = '\0';
	return sol - start;
}

// Lex up a single line from the diskstats file.
static int
lex_diskstats(const char *sol, const char *eol, diskstats *dstat) {
	int consumed;

	consumed = lex_diskstats_prefix(sol, eol, dstat);
	if(consumed < 0){
		return -1;
	}
	sol += consumed;
	// sectorsRead is f3, sectorsWritten is f7
	uintmax_t f1, f2, f3, f4, f5, f6, f7;
	consumed = sscanf(sol, "%ju %ju %ju %ju %ju %ju %ju",
			  &f1, &f2, &f3, &f4, &f5, &f6, &f7);
	if(consumed != 7){
		return -1;
	}
	dstat->total.sectors_read = f3;
	dstat->total.sectors_written = f7;
	return 0;
}

int read_diskstats(const char *path, diskstats **stats) {
	diskstats *tmpstats;
	size_t buflen;
	char *buf;

	if((buf = read_procfs_file(path, &buflen)) == NULL){
		return -1;
	}
	size_t offset = 0; // where our line starts in the file
	size_t eol; // points one past last byte of line after find_line_end()
	int devices = 0;
	*stats = NULL;
	tmpstats = NULL;
	while((eol = find_line_end(buf, offset, buflen)) > offset){
		diskstats dstat;
		memset(&dstat, 0, sizeof(dstat));
		if(lex_diskstats(buf + offset, buf + eol, &dstat)){
			free(tmpstats);
			free(buf);
			return -1;
		}
		if(add_diskstat(&tmpstats, devices, &dstat)){
			free(tmpstats);
			free(buf);
			return -1;
		}
		++devices;
		offset = eol + 1;
	}
	free(buf);
	*stats = tmpstats;
	return devices;
}

// copyright 2012–2020 nick black
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "mmap.h"
#include "growlight.h"

// Handle files which aren't easily supported by mmap(), such as /proc entries
// which don't return their true lengths to fstat() and friends. fd should be
// positioned at the beginning of the file. We use anonymous mappings rather
// than malloc() so that we can pass everything back to munmap().
static void *
read_map_virt_fd(int fd, off_t *len){
  int pgsize = getpagesize();
  char *buf = malloc(pgsize);
  void *map = MAP_FAILED;
  ssize_t r;
  *len = 0;

  if(pgsize <= 0){
    diag("Invalid pagesize: %d (%s?)\n", pgsize, strerror(errno));
    free(buf);
    return MAP_FAILED;
  }
  size_t mapsize = 0;
  while((r = read(fd, buf, sizeof(buf))) > 0){
    size_t size = (*len + r + (pgsize - 1)) / pgsize * pgsize;
    if(map == MAP_FAILED){
      map = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    }else if(mapsize != size){
      void* tmp = mremap(map, mapsize, size, MREMAP_MAYMOVE);
      if(tmp == MAP_FAILED){
        munmap(map, mapsize);
      }
      map = tmp;
    }
    if(map == MAP_FAILED){
      goto maperr;
    }
    mapsize = size;
//fprintf(stderr, "read buf: [%s] r: %zd len: %d\n", buf, r, (int)*len);
    memcpy((char *)map + *len, buf, r);
    *len += r;
//fprintf(stderr, "***[%.*s]***\n", (int)*len, (char*)map);
  }
  if(r < 0){
    int e = errno;
    diag("Error reading %d (%s?)\n", fd, strerror(errno));
    if(map != MAP_FAILED){
      munmap(map, mapsize);
    }
    errno = e;
    free(buf);
    return MAP_FAILED;
  }
  if(*len == 0){
    *len = pgsize;
    map = mmap(NULL, *len, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if(map == MAP_FAILED){
      goto maperr;
    }
  }
//fprintf(stderr, "***[%.*s]***\n", (int)*len, (char*)map);
  return map;

maperr:
  diag("Couldn't extend map of %d past %ju (%s?)\n", fd, (uintmax_t)*len, strerror(errno));
  *len = -1;
  free(buf);
  return MAP_FAILED;
}

void *map_virt_fd(int fd, off_t *len){
  struct stat st;
  void *map;

//fprintf(stderr, "fd %d %d\n", fd, (int)*len);
  if(fstat(fd, &st)){
    diag("Couldn't get size of fd %d (%s?)\n", fd, strerror(errno));
    return MAP_FAILED;
  }
  // Most /proc entries return a 0 length
  if((*len = st.st_size) == 0){
    return read_map_virt_fd(fd, len);
  }
//fprintf(stderr, "READ MAP MMaP, %d\n", (int)*len);
  if((map = mmap(NULL, *len, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED){
    int e = errno;
    diag("Couldn't map %ju at %d (%s?)\n",
        (uintmax_t)*len, fd, strerror(errno));
    errno = e;
    return MAP_FAILED;
  }
  return map;
}

void *map_virt_file(const char *fn, int *fd, off_t *len){
  void *map;
  int tfd;

//fprintf(stderr, "OPENING [%s]\n", fn);
  if((tfd = open(fn, O_RDONLY|O_NONBLOCK|O_CLOEXEC)) < 0){
    int e = errno;
    diag("Couldn't open %s (%s?)\n", fn, strerror(errno));
    errno = e;
    return MAP_FAILED;
  }
  if((map = map_virt_fd(tfd, len)) == MAP_FAILED){
    close(tfd);
    return MAP_FAILED;
  }
  *fd = tfd;
  return map;
}

int munmap_virt(void *map, off_t len){
  return munmap(map, len);
}

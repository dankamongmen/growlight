// copyright 2012–2021 nick black
#ifndef GROWLIGHT_MMAP
#define GROWLIGHT_MMAP

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/mman.h>

void *map_virt_fd(int,off_t *);
void *map_virt_file(const char *,int *,off_t *);
int munmap_virt(void *,off_t);

#ifdef __cplusplus
}
#endif

#endif

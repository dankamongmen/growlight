#ifndef GROWLIGHT_LIBBLKID
#define GROWLIGHT_LIBBLKID

#ifdef __cplusplus
extern "C" {
#endif

#include <blkid/blkid.h>

struct device;

int probe_blkid_dev(const char *,blkid_probe *);
int probe_blkid_superblock(const char *,struct device *);
int close_blkid(void);

#ifdef __cplusplus
}
#endif

#endif

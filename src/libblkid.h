#ifndef GROWLIGHT_LIBBLKID
#define GROWLIGHT_LIBBLKID

#ifdef __cplusplus
extern "C" {
#endif

#include <blkid/blkid.h>

int probe_blkid_dev(const char *,blkid_probe *);
int load_blkid_superblocks(void);
int close_blkid(void);

#ifdef __cplusplus
}
#endif

#endif

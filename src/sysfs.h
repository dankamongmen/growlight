#ifndef GROWLIGHT_SYSFS
#define GROWLIGHT_SYSFS

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

int sysfs_devno(int,dev_t *);
unsigned sysfs_exist_p(int,const char *);
char *get_sysfs_string(int,const char *);
int get_sysfs_bool(int,const char *,unsigned *);

#ifdef __cplusplus
}
#endif

#endif

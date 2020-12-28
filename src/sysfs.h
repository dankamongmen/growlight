// copyright 2012–2021 nick black
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
int get_sysfs_int(int,const char *,int *);
int get_sysfs_uint(int,const char *,unsigned long *);
int write_sysfs(const char *,const char *);

#ifdef __cplusplus
}
#endif

#endif

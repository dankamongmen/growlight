#ifndef GROWLIGHT_ZFS
#define GROWLIGHT_ZFS

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

int init_zfs_support(void);
int stop_zfs_support(void);
int print_zfs_version(FILE *);

#ifdef __cplusplus
}
#endif

#endif

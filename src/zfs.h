#ifndef GROWLIGHT_ZFS
#define GROWLIGHT_ZFS

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "growlight.h"

int init_zfs_support(const glightui *);
int stop_zfs_support(void);
int scan_zpools(const glightui *);
int print_zfs_version(FILE *);

#ifdef __cplusplus
}
#endif

#endif

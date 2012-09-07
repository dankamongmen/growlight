#ifndef GROWLIGHT_ZFS
#define GROWLIGHT_ZFS

#ifdef __cplusplus
extern "C" {
#endif

struct device;

#include <stdio.h>
#include "growlight.h"

int init_zfs_support(const glightui *);
int stop_zfs_support(void);
int scan_zpools(const glightui *);
int print_zfs_version(FILE *);
int destroy_zpool(struct device *);

int make_zmirror(const char *,char * const *,int);
int make_raidz1(const char *,char * const *,int);
int make_raidz2(const char *,char * const *,int);
int make_raidz3(const char *,char * const *,int);

// Make a zpool from a single device (not recommended)
int make_zfs(const char *,const char *,int);

#ifdef __cplusplus
}
#endif

#endif

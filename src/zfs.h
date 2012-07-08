#ifndef GROWLIGHT_ZFS
#define GROWLIGHT_ZFS

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

struct growlight_ui;

int init_zfs_support(const struct growlight_ui *);
int stop_zfs_support(void);
int scan_zpools(const struct growlight_ui *);
int print_zfs_version(FILE *);

#ifdef __cplusplus
}
#endif

#endif

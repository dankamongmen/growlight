#ifndef GROWLIGHT_PTABLE
#define GROWLIGHT_PTABLE

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Create the given type of partition table on this device
int explore_md_sysfs(struct device *,const char *);

#ifdef __cplusplus
}
#endif

#endif

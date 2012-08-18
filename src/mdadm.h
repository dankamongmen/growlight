#ifndef GROWLIGHT_MDADM
#define GROWLIGHT_MDADM

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Wants a dirfd corresponding to the md/ sysfs directory for the node
int explore_md_sysfs(struct device *,int);

int destroy_mdadm(struct device *);

#ifdef __cplusplus
}
#endif

#endif

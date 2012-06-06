#ifndef GROWLIGHT_MDADM
#define GROWLIGHT_MDADM

#ifdef __cplusplus
extern "C" {
#endif

// Wants a dirfd corresponding to the md/ sysfs directory for the node
int explore_md_sysfs(int);

#ifdef __cplusplus
}
#endif

#endif

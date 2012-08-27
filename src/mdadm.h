#ifndef GROWLIGHT_MDADM
#define GROWLIGHT_MDADM

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Wants a dirfd corresponding to the md/ sysfs directory for the node
int explore_md_sysfs(struct device *,int);

int destroy_mdadm(struct device *);

int make_mdraid0(const char *name,char * const *,int);
int make_mdraid1(const char *name,char * const *,int);
int make_mdraid4(const char *name,char * const *,int);
int make_mdraid5(const char *name,char * const *,int);
int make_mdraid6(const char *name,char * const *,int);

#ifdef __cplusplus
}
#endif

#endif

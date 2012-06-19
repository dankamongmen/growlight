#ifndef GROWLIGHT_PTABLE
#define GROWLIGHT_PTABLE

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Create the given type of partition table on this device
int make_partition_table(struct device *,const char *);
int wipe_ptable(struct device *);

int add_partition(struct device *,const wchar_t *,size_t);
int wipe_partition(struct device *);
int name_partition(struct device *,const wchar_t *);
int check_partition(struct device *);

#ifdef __cplusplus
}
#endif

#endif

#ifndef GROWLIGHT_PTABLE
#define GROWLIGHT_PTABLE

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct device;

// Create the given type of partition table on this device
int make_partition_table(struct device *,const char *);

// Wipe the partition table (make it unrecognizable, preferably by overwriting
// it with zeroes). If a ptype is specified, it is assumed that this partition
// table type is being used, and we will zero out according to the specified
// type, even if it doesn't match the detected type (very dangerous!). If no
// type is specified, the detected type, if it exists, is used.
int wipe_ptable(struct device *,const char *);

int add_partition(struct device *,const wchar_t *,uintmax_t,unsigned long long);
int add_partition_precise(struct device *,const wchar_t *,uintmax_t,uintmax_t,unsigned long long);
int wipe_partition(const struct device *);
int name_partition(struct device *,const wchar_t *);
int uuid_partition(struct device *,const void *);
int check_partition(struct device *);
int partition_set_flag(struct device *,uint64_t,unsigned);
int partition_set_code(struct device *,unsigned long long);
uintmax_t lookup_last_usable_sector(const struct device *);
int partitions_named_p(const struct device *);

#ifdef __cplusplus
}
#endif

#endif

#ifndef GROWLIGHT_MSDOS
#define GROWLIGHT_MSDOS

#ifdef __cplusplus
extern "C" {
#endif

#include <wchar.h>
#include <stdint.h>

struct device;

// Pass the block device
int new_msdos(struct device *);
int zap_msdos(struct device *);
int add_msdos(struct device *,const wchar_t *,uintmax_t,uintmax_t,unsigned long long);

// Pass the partition
int del_msdos(const struct device *);
int flag_msdos(struct device *,uint64_t,unsigned);
int flags_msdos(struct device *,uint64_t);
int code_msdos(struct device *,unsigned long long);

uintmax_t first_msdos(const struct device *);
uintmax_t last_msdos(const struct device *);

#ifdef __cplusplus
}
#endif

#endif

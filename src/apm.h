// copyright 2012–2020 nick black
#ifndef GROWLIGHT_APM
#define GROWLIGHT_APM

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct device;

// Pass the block device
int new_apm(struct device *);
int zap_apm(struct device *);

uintmax_t first_apm(const struct device *);
uintmax_t last_apm(const struct device *);

#ifdef __cplusplus
}
#endif

#endif

// copyright 2012–2021 nick black
#ifndef GROWLIGHT_HEALTH
#define GROWLIGHT_HEALTH

#ifdef __cplusplus
extern "C" {
#endif

struct device;

int badblock_scan(struct device *,unsigned);

#ifdef __cplusplus
}
#endif

#endif

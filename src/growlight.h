#ifndef GROWLIGHT_GROWLIGHT
#define GROWLIGHT_GROWLIGHT

#ifdef __cplusplus
extern "C" {
#endif

struct device;

struct device *lookup_device(const char *);

#ifdef __cplusplus
}
#endif

#endif

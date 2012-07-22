#ifndef GROWLIGHT_FS
#define GROWLIGHT_FS

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Create the given type of filesystem on this device
int make_filesystem(struct device *,const char *);

#ifdef __cplusplus
}
#endif

#endif

#ifndef GROWLIGHT_TARGET
#define GROWLIGHT_TARGET

#ifdef __cplusplus
extern "C" {
#endif

struct device;

int prepare_mount(struct device *,const char *,const char *,const char *);

extern const char *growlight_target;

#ifdef __cplusplus
}
#endif

#endif

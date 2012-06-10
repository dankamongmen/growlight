#ifndef GROWLIGHT_SG
#define GROWLIGHT_SG

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Takes an open file descriptor on the device node
int sg_interrogate(struct device *,int);

#ifdef __cplusplus
}
#endif

#endif

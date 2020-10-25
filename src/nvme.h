// copyright 2012–2020 nick black
#ifndef GROWLIGHT_NVME
#define GROWLIGHT_NVME

#ifdef __cplusplus
extern "C" {
#endif

struct device;

int nvme_interrogate(struct device *, int sd);

#ifdef __cplusplus
}
#endif

#endif

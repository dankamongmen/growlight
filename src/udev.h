#ifndef GROWLIGHT_UDEV
#define GROWLIGHT_UDEV

#ifdef __cplusplus
extern "C" {
#endif

#include "growlight.h"

int monitor_udev(void);
int udev_event(const glightui *);
int shutdown_udev(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef GROWLIGHT_UDEV
#define GROWLIGHT_UDEV

#ifdef __cplusplus
extern "C" {
#endif

int monitor_udev(void);
int udev_event(void);
int shutdown_udev(void);

#ifdef __cplusplus
}
#endif

#endif

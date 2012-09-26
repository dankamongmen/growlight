#ifndef GROWLIGHT_DMI
#define GROWLIGHT_DMI

#ifdef __cplusplus
extern "C" {
#endif

// Load the DMI configuration from sysfs
int dmi_init(void);

#ifdef __cplusplus
}
#endif

#endif

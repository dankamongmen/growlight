#ifndef GROWLIGHT_DMI
#define GROWLIGHT_DMI

#ifdef __cplusplus
extern "C" {
#endif

// Load the DMI configuration from sysfs
int dmi_init(void);

const char *get_bios_version(void);
const char *get_bios_vendor(void);

#ifdef __cplusplus
}
#endif

#endif

// copyright 2012–2020 nick black
#ifndef GROWLIGHT_SSD
#define GROWLIGHT_SSD

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Run the fstrim(8) command on a mounted filesystem
int fstrim(const char *);

// Run fstrim() on all a device's mounts.
int fstrim_dev(struct device *);

#ifdef __cplusplus
}
#endif

#endif

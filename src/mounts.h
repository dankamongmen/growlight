#ifndef GROWLIGHT_MOUNTS
#define GROWLIGHT_MOUNTS

#ifdef __cplusplus
extern "C" {
#endif

struct device;
struct controller;
struct growlight_ui;

// (Re)parse the specified file having /proc/mounts format. Remember that
// /proc/mounts must be poll()ed with POLLPRI, not POLLIN!
int parse_mounts(const struct growlight_ui *,const char *);
int mmount(struct device *,const char *,const char *);
int unmount(struct device *,const char *);
void clear_mounts(struct controller *);

#ifdef __cplusplus
}
#endif

#endif

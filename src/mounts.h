#ifndef GROWLIGHT_MOUNTS
#define GROWLIGHT_MOUNTS

#ifdef __cplusplus
extern "C" {
#endif

struct device;
struct controller;
struct growlight_ui;

typedef struct mntentry {
	char *path;
	char *dev;
	char *ops;
	char *label;
	char *uuid;
} mntentry;

// (Re)parse the specified file having /proc/mounts format. Remember that
// /proc/mounts must be poll()ed with POLLPRI, not POLLIN!
int parse_mounts(const struct growlight_ui *,const char *);
int unmount(struct device *);
void clear_mounts(struct controller *);

#ifdef __cplusplus
}
#endif

#endif

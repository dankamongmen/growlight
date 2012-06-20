#ifndef GROWLIGHT_MOUNTS
#define GROWLIGHT_MOUNTS

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mntentry {
	char *path;
	char *dev;
	char *fs;
	char *ops;
	char *label;
	char *uuid;
} mntentry;

// (Re)parse the specified file having /proc/mounts format. Remember that
// /proc/mounts must be poll()ed with POLLPRI, not POLLIN!
int parse_mounts(const char *);

#ifdef __cplusplus
}
#endif

#endif

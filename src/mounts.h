#ifndef GROWLIGHT_MOUNTS
#define GROWLIGHT_MOUNTS

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mount {
	char *path;
	char *dev;
	char *fs;
	char *ops;
} mount;

// (Re)parse the specified file having /proc/mounts format. Remember that
// /proc/mounts must be poll()ed with POLLPRI, not POLLIN!
int parse_mounts(const char *);

#ifdef __cplusplus
}
#endif

#endif

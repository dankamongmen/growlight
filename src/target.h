#ifndef GROWLIGHT_TARGET
#define GROWLIGHT_TARGET

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

struct device;

int set_target(const char *);
int finalize_target(void);
char *dump_targets(void);

// don't muck around with me externally. use set_target() and get_target().
extern const char *growlight_target;

static inline const char *
get_target(void){
	return growlight_target;
}

// Indicate that we've just mounted/unmounted the target root
int mount_target(void);
int unmount_target(void);

#ifdef __cplusplus
}
#endif

#endif

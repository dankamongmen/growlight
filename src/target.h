#ifndef GROWLIGHT_TARGET
#define GROWLIGHT_TARGET

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

struct device;

int prepare_mount(struct device *,const char *,const char *,const char *);

int set_target(const char *);
int finalize_target(void);
int dump_targets(FILE *);

// don't go mucking around with me externally. use set_target().
extern const char *growlight_target;

static inline const char *
get_target(void){
	return growlight_target;
}

#ifdef __cplusplus
}
#endif

#endif

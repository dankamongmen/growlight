#ifndef GROWLIGHT_AGGREGATE
#define GROWLIGHT_AGGREGATE

#include "growlight.h"

static inline int
aggregate_default_p(const char *aggtype){
	return !strcmp(aggtype,"raidz2");
}

const aggregate_type *get_aggregate(const char *);

#endif

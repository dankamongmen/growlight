#ifndef GROWLIGHT_AGGREGATE
#define GROWLIGHT_AGGREGATE

static inline int
aggregate_default_p(const char *aggtype){
	return !strcmp(aggtype,"raidz2");
}

#endif

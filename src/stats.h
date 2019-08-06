#ifndef GROWLIGHT_STATS
#define GROWLIGHT_STATS

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stdint.h>

// See Linux's documentation/iostats.txt for description of the procfs disk
// statistics. On Linux 4.18+, we have 17 fields:
//
// major minor devname
// readsComp readsMerged sectorsRead msRead
// writesComp writesMerged sectorsWritten msWritten
// iosInProgress msIOs weightedmsIOs
// discardsComp discardsMerged sectorsDiscarded msDiscarded
//
// Prior to 4.18, the last four fields were not present.
typedef struct statpack {
	uint64_t sectors_read;
	uint64_t sectors_written;
} statpack;

typedef struct diskstats {
	char name[NAME_MAX + 1];
	statpack raw;
	statpack delta;
} diskstats;

// Reads the entirety of /proc/diskstats, and copies the results we care about
// into a heap-allocated array of stats objects. We use /proc/diskstats because
// we'd otherwise need open a sysfs file per partition/block device. If an
// array of diskstats is passed in, its entries will be used to prepare the
// deltas of the new array. prev must be NULL iff prevcount == 0. The return
// value is the number of entries in *stats. *stats is NULL iff the return
// value is less than or equal to 0. An error results in a negative return.
//
// If a device is removed between two calls to read_stats(), its entry in the
// prev array ought be 0d out, or spliced out, to guarantee correct deltas on
// the subsequent call (in case another device is added and gets its name).
int read_proc_diskstats(diskstats *prev, int prevcount, diskstats **stats);

// Allows the path to be specified.
int read_diskstats(const char *path, diskstats *prev, int prevcount, diskstats **stats);

#ifdef __cplusplus
}
#endif

#endif

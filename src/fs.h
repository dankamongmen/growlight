#ifndef GROWLIGHT_FS
#define GROWLIGHT_FS

#ifdef __cplusplus
extern "C" {
#endif

struct device;
struct growlight_ui;

#include <string.h>
#include <stdint.h>

// Create the given type of filesystem on this device
int make_filesystem(struct device *,const char *,const char *);
int parse_filesystems(const struct growlight_ui *,const char *);
int wipe_filesystem(struct device *);

static inline int
fstype_default_p(const char *fstype){
	return !strcmp(fstype,"ext4");
}

static inline int
aggregate_fs_p(const char *fstype){
	return !strcmp(fstype,"linux_raid_member") ||
		!strcmp(fstype,"zfs_member");
}

struct mkfsmarshal {
	const char *name;	// supply this label, if possible
	int force;		// supply a force directive, if one exists
	uintmax_t stride;	// opt. for raid array of strideB chunks
	uintmax_t swidth;	// opt. for raid array of swidth stripe width
};

// Does the filesystem support the concept of a name/label?
int fstype_named_p(const char *);

// Is the filesystem virtual (not backed by a single device entry)?
int fstype_virt_p(const char *);

#ifdef __cplusplus
}
#endif

#endif

#ifndef GROWLIGHT_FS
#define GROWLIGHT_FS

#ifdef __cplusplus
extern "C" {
#endif

struct device;
struct growlight_ui;

// Create the given type of filesystem on this device
int make_filesystem(struct device *,const char *,const char *);
int parse_filesystems(const struct growlight_ui *,const char *);
int wipe_filesystem(struct device *);

static inline int
fstype_default_p(const char *fstype){
	return !strcmp(fstype,"ext4");
}

int fstype_named_p(const char *);

#ifdef __cplusplus
}
#endif

#endif

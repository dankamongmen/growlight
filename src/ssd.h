#ifndef GROWLIGHT_SSD
#define GROWLIGHT_SSD

#ifdef __cplusplus
extern "C" {
#endif

// Run the fstrim(8) command on a mounted filesystem
int fstrim(const char *);

#ifdef __cplusplus
}
#endif

#endif

// copyright 2012–2021 nick black
#ifndef GROWLIGHT_MBR
#define GROWLIGHT_MBR

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Take a SHA-1 checksum over the MBR code area. fd is an open fd for a true
// block device. The buffer must be able to hold 20 bytes (160 bits). The
// checksum is taken over the first 444 bytes, not all 512 bytes of the MBR.
int mbrsha1(struct device *, int, void *);

int zerombrp(const void *);

int wipe_biosboot(struct device *);
int wipe_dosmbr(struct device *);
int wipe_dos_ptable(struct device *);

#ifdef __cplusplus
}
#endif

#endif

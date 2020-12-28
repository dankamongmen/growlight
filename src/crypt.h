// copyright 2012–2021 nick black
#ifndef GROWLIGHT_CRYPT
#define GROWLIGHT_CRYPT

#ifdef __cplusplus
extern "C" {
#endif

struct device;

// Create LUKS on the device
int cryptondev(struct device *);

int crypt_start(void);
int crypt_stop(void);

#ifdef __cplusplus
}
#endif

#endif

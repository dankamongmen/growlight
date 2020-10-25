// copyright 2012–2020 nick black
#ifndef GROWLIGHT_SECURE
#define GROWLIGHT_SECURE

#ifdef __cplusplus
extern "C" {
#endif

struct device;

int ata_secure_erase(struct device *);

#ifdef __cplusplus
}
#endif

#endif

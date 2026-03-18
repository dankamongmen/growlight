// copyright 2012–2021 nick black
#ifndef GROWLIGHT_SHA
#define GROWLIGHT_SHA

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void sha1(const void* src, size_t len, uint8_t* dst);

#ifdef __cplusplus
}
#endif

#endif

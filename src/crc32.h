#ifndef GROWLIGHT_CRC32
#define GROWLIGHT_CRC32

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

uint32_t crc32(const void *,size_t);

#ifdef __cplusplus
}
#endif

#endif

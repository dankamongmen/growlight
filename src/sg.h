// copyright 2012–2020 nick black
#ifndef GROWLIGHT_SG
#define GROWLIGHT_SG

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct device;

// Takes an open file descriptor on the device node
int sg_interrogate(struct device *, int);

// Take the incoming serial number and trim leading, repeated, or trailing
// whitespace. The serial number may or may not be NUL-terminated (don't blame
// me; it's how the ioctls work). A NUL-terminator must be respected, but if
// none is present in serialmax characters, stop. Returns a heap-allocated
// buffer of not more than serialmax + 1 bytes, containing the NUL-terminated
// and cleaned up serial number.
void *cleanup_serial(const void *serial, size_t serialmax);

#ifdef __cplusplus
}
#endif

#endif

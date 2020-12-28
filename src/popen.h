// copyright 2012–2021 nick black
#ifndef GROWLIGHT_POPEN
#define GROWLIGHT_POPEN

#ifdef __cplusplus
extern "C" {
#endif

#include <wchar.h>

int popen_drain(const char *);
int vpopen_drain(const char *,wchar_t * const *);
int vspopen_drain(const char *,...) __attribute__ ((format (printf,1,2)));

#ifdef __cplusplus
}
#endif

#endif

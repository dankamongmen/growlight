#ifndef GROWLIGHT_POPEN
#define GROWLIGHT_POPEN

#ifdef __cplusplus
extern "C" {
#endif

#include <wchar.h>

int popen_drain(const char *);
int vpopen_drain(const char *,wchar_t * const *);

#ifdef __cplusplus
}
#endif

#endif

#ifndef GROWLIGHT_POPEN
#define GROWLIGHT_POPEN

#ifdef __cplusplus
extern "C" {
#endif

int popen_drain(const char *);
int vpopen_drain(const char *,...);

#ifdef __cplusplus
}
#endif

#endif

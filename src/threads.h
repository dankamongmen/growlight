// copyright 2012–2021 nick black
#ifndef GROWLIGHT_THREADS
#define GROWLIGHT_THREADS

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

int recursive_lock_init(pthread_mutex_t *lock);

#ifdef __cplusplus
}
#endif

#endif

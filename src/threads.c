// copyright 2012–2021 nick black
#include "threads.h"

// initialize a recursive mutex lock in a way that works on both glibc + musl
int recursive_lock_init(pthread_mutex_t *lock){
#ifndef __GLIBC__
#define PTHREAD_MUTEX_RECURSIVE_NP PTHREAD_MUTEX_RECURSIVE
#endif
	pthread_mutexattr_t attr;
	if(pthread_mutexattr_init(&attr)){
		return -1;
	}
	if(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP)){
		pthread_mutexattr_destroy(&attr);
		return -1;
	}
	if(pthread_mutex_init(lock, &attr)){
		pthread_mutexattr_destroy(&attr);
		return -1;
	}
  pthread_mutexattr_destroy(&attr);
	return 0;
#ifndef __GLIBC__
#undef PTHREAD_MUTEX_RECURSIVE_NP
#endif
}

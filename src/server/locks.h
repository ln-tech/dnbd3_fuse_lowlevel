#ifndef _LOCKS_H_
#define _LOCKS_H_

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _DEBUG

#define mutex_init( lock ) debug_mutex_init( #lock, __FILE__, __LINE__, lock)
#define mutex_lock( lock ) debug_mutex_lock( #lock, __FILE__, __LINE__, lock)
#define mutex_trylock( lock ) debug_mutex_trylock( #lock, __FILE__, __LINE__, lock)
#define mutex_unlock( lock ) debug_mutex_unlock( #lock, __FILE__, __LINE__, lock)
#define mutex_cond_wait( cond, lock ) debug_mutex_cond_wait( #lock, __FILE__, __LINE__, cond, lock)
#define mutex_destroy( lock ) debug_mutex_destroy( #lock, __FILE__, __LINE__, lock)

int debug_mutex_init(const char *name, const char *file, int line, pthread_mutex_t *lock);
int debug_mutex_lock(const char *name, const char *file, int line, pthread_mutex_t *lock);
int debug_mutex_trylock(const char *name, const char *file, int line, pthread_mutex_t *lock);
int debug_mutex_unlock(const char *name, const char *file, int line, pthread_mutex_t *lock);
int debug_mutex_cond_wait(const char *name, const char *file, int line, pthread_cond_t *restrict cond, pthread_mutex_t *restrict lock);
int debug_mutex_destroy(const char *name, const char *file, int line, pthread_mutex_t *lock);

void debug_dump_lock_stats();


#else

#define mutex_init( lock ) pthread_mutex_init(lock, NULL)
#define mutex_lock( lock ) pthread_mutex_lock(lock)
#define mutex_trylock( lock ) pthread_mutex_trylock(lock)
#define mutex_unlock( lock ) pthread_mutex_unlock(lock)
#define mutex_cond_wait( cond, lock ) pthread_cond_wait(cond, lock)
#define mutex_destroy( lock ) pthread_mutex_destroy(lock)

#endif

#ifdef DEBUG_THREADS

extern int debugThreadCount;
#define thread_create(thread,attr,routine,arg) (logadd( LOG_THREAD CREATE, "%d @ %s:%d\n", debugThreadCount, __FILE__, (int)__LINE__), debug_thread_create(thread, attr, routine, arg))
static inline pthread_t debug_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg)
{
	int i;
	if (attr == NULL || pthread_attr_getdetachstate(attr, &i) != 0 || i == PTHREAD_CREATE_JOINABLE) {
		++debugThreadCount;
	}
	return pthread_create( thread, attr, start_routine, arg );
}

#define thread_detach(thread) (logadd( LOG_THREAD DETACH, "%d @ %s:%d\n", debugThreadCount, __FILE__, __LINE__), debug_thread_detach(thread))
static inline int debug_thread_detach(pthread_t thread)
{
	const int ret = pthread_detach(thread);
	if (ret == 0) {
		--debugThreadCount;
	} else {
		logadd( LOG_THREAD DETACH, "Tried to detach invalid thread (error %d)\n", (int)errno);
		exit(1);
	}
	return ret;
}
#define thread_join(thread,value) (logadd( LOG_THREAD JOIN, "%d @ %s:%d\n", debugThreadCount, __FILE__, __LINE__), debug_thread_join(thread,value))
static inline int debug_thread_join(pthread_t thread, void **value_ptr)
{
	const int ret = pthread_join(thread, value_ptr);
	if (ret == 0) {
		--debugThreadCount;
	} else {
		logadd( LOG_THREAD JOIN, "Tried to join invalid thread (error %d)\n", (int)errno);
		exit(1);
	}
	return ret;
}

#else

#define thread_create(thread,attr,routine,param)  pthread_create( thread, attr, routine, param )
#define thread_detach(thread) pthread_detach( thread )
#define thread_join(thread,value) pthread_join( thread, value )

#endif

void debug_locks_start_watchdog();
void debug_locks_stop_watchdog();

#endif /* LOCKS_H_ */

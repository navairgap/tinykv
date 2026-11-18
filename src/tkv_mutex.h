/* Internal handle locking. Threads are on by default; embedders who
 * serialize access themselves (or have no pthreads) build with
 * -DTINYKV_NO_THREADS to drop the dependency entirely.
 *
 * The mutex is recursive so public entry points can layer (iteration
 * re-reads values through tinykv_get) without re-entrant deadlocks. */
#ifndef TINYKV_TKV_MUTEX_H
#define TINYKV_TKV_MUTEX_H

#if defined(TINYKV_NO_THREADS)

typedef struct { char unused; } tkv_mutex;
static inline int tkv_mutex_init(tkv_mutex *mutex) { (void)mutex; return 0; }
static inline void tkv_mutex_lock(tkv_mutex *mutex) { (void)mutex; }
static inline void tkv_mutex_unlock(tkv_mutex *mutex) { (void)mutex; }
static inline void tkv_mutex_destroy(tkv_mutex *mutex) { (void)mutex; }

#else

#include <pthread.h>

typedef pthread_mutex_t tkv_mutex;

static inline int tkv_mutex_init(tkv_mutex *mutex)
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    return pthread_mutex_init(mutex, &attributes) == 0 ? 0 : -1;
}
static inline void tkv_mutex_lock(tkv_mutex *mutex)
{
    (void)pthread_mutex_lock(mutex);
}
static inline void tkv_mutex_unlock(tkv_mutex *mutex)
{
    (void)pthread_mutex_unlock(mutex);
}
static inline void tkv_mutex_destroy(tkv_mutex *mutex)
{
    (void)pthread_mutex_destroy(mutex);
}

#endif
#endif

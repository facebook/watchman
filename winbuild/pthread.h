/*
 * Posix Threads library for Microsoft Windows
 *
 * Use at own risk, there is no implied warranty to this code.
 * It uses undocumented features of Microsoft Windows that can change
 * at any time in the future.
 *
 * (C) 2010 Lockless Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, * are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of Lockless Inc. nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AN ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * You may want to use the MingW64 winpthreads library instead.
 * It is based on this, but adds error checking.
 */

/*
 * Version 1.0.1 Released 2 Feb 2012
 * Fixes pthread_barrier_destroy() to wait for threads to exit the barrier.
 */

#ifndef WIN_PTHREADS
#define WIN_PTHREADS


#include <windows.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/timeb.h>

#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ENOTSUP
#define ENOTSUP   134
#endif

#define PTHREAD_CANCEL_DISABLE 0
#define PTHREAD_CANCEL_ENABLE 0x01

#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 0x02

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 0x04

#define PTHREAD_EXPLICT_SCHED 0
#define PTHREAD_INHERIT_SCHED 0x08

#define PTHREAD_SCOPE_PROCESS 0
#define PTHREAD_SCOPE_SYSTEM 0x10

#define PTHREAD_DEFAULT_ATTR (PTHREAD_CANCEL_ENABLE)

#define PTHREAD_CANCELED ((void *)(intptr_t)0xDEADBEEF)

#define PTHREAD_ONCE_INIT 0
#define PTHREAD_RWLOCK_INITIALIZER {0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_SPINLOCK_INITIALIZER 0
#define _PTHREAD_MUTEX_CS_INITIALIZER \
  { (PRTL_CRITICAL_SECTION_DEBUG)(intptr_t) - 1 - 1, -1, 0, 0, 0, 0 }
#define PTHREAD_MUTEX_INITIALIZER \
  {PTHREAD_SPINLOCK_INITIALIZER, 0, _PTHREAD_MUTEX_CS_INITIALIZER}
#define PTHREAD_BARRIER_INITIALIZER \
  {0,0,PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER}



#define PTHREAD_DESTRUCTOR_ITERATIONS 256
#define PTHREAD_KEYS_MAX (1<<20)

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define PTHREAD_MUTEX_DEFAULT 3
#define PTHREAD_MUTEX_SHARED 4
#define PTHREAD_MUTEX_PRIVATE 0
#define PTHREAD_PRIO_NONE 0
#define PTHREAD_PRIO_INHERIT 8
#define PTHREAD_PRIO_PROTECT 16
#define PTHREAD_PRIO_MULT 32
#define PTHREAD_PROCESS_SHARED 0
#define PTHREAD_PROCESS_PRIVATE 1

#define PTHREAD_BARRIER_SERIAL_THREAD 1


typedef struct _pthread_cleanup _pthread_cleanup;
struct _pthread_cleanup
{
  void (*func)(void *);
  void *arg;
  _pthread_cleanup *next;
};

typedef unsigned pthread_key_t;

struct _pthread_v
{
  void *ret_arg;
  void *(* func)(void *);
  _pthread_cleanup *clean;
  HANDLE h;
  int cancelled;
  unsigned p_state;
  pthread_key_t keymax;
  void **keyval;

  char pad[8];
  jmp_buf jb;
};

typedef struct _pthread_v *pthread_t;

typedef struct pthread_barrier_t pthread_barrier_t;
struct pthread_barrier_t
{
  int count;
  int total;
  CRITICAL_SECTION m;
  CONDITION_VARIABLE cv;
};

typedef struct pthread_attr_t pthread_attr_t;
struct pthread_attr_t
{
  unsigned p_state;
  void *stack;
  size_t s_size;
};

typedef long pthread_once_t;
typedef unsigned pthread_mutexattr_t;
typedef SRWLOCK pthread_rwlock_t;
typedef void *pthread_barrierattr_t;
typedef long pthread_spinlock_t;
typedef int pthread_condattr_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef int pthread_rwlockattr_t;

typedef struct pthread_mutex_t pthread_mutex_t;
struct pthread_mutex_t {
   pthread_spinlock_t initializer_spin_lock;
   BOOL initialized;
   CRITICAL_SECTION cs;
};

#define pthread_cleanup_push(F, A)\
{\
  const _pthread_cleanup _pthread_cup = {(F), (A), pthread_self()->clean};\
  _ReadWriteBarrier();\
  pthread_self()->clean = (_pthread_cleanup *) &_pthread_cup;\
  _ReadWriteBarrier()

/* Note that if async cancelling is used, then there is a race here */
#define pthread_cleanup_pop(E)\
  (pthread_self()->clean = _pthread_cup.next, \
   (E?_pthread_cup.func(_pthread_cup.arg),0:0));}

pthread_t pthread_self(void);
int pthread_once(pthread_once_t *o, void (*func)(void));
int _pthread_once_raw(pthread_once_t *o, void (*func)(void));
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_init(pthread_mutex_t *m, pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);

#define pthread_mutex_getprioceiling(M, P) ENOTSUP
#define pthread_mutex_setprioceiling(M, P) ENOTSUP

int pthread_equal(pthread_t t1, pthread_t t2);
void pthread_testcancel(void);
int pthread_rwlock_init(pthread_rwlock_t *l, pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
pthread_t pthread_self(void);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);

int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *ts);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *ts);

#define pthread_getschedparam(T, P, S) ENOTSUP
#define pthread_setschedparam(T, P, S) ENOTSUP
#define pthread_getcpuclockid(T, C) ENOTSUP

int pthread_exit(void *res);
void pthread_testcancel(void);
int pthread_cancel(pthread_t t);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *a, int flag);
int pthread_attr_getdetachstate(pthread_attr_t *a, int *flag);
int pthread_attr_setinheritsched(pthread_attr_t *a, int flag);
int pthread_attr_getinheritsched(pthread_attr_t *a, int *flag);
int pthread_attr_setscope(pthread_attr_t *a, int flag);
int pthread_attr_getscope(pthread_attr_t *a, int *flag);
int pthread_attr_getstackaddr(pthread_attr_t *attr, void **stack);
int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stack);
int pthread_attr_getstacksize(pthread_attr_t *attr, size_t *size);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);

#define pthread_attr_getguardsize(A, S) ENOTSUP
#define pthread_attr_setgaurdsize(A, S) ENOTSUP
#define pthread_attr_getschedparam(A, S) ENOTSUP
#define pthread_attr_setschedparam(A, S) ENOTSUP
#define pthread_attr_getschedpolicy(A, S) ENOTSUP
#define pthread_attr_setschedpolicy(A, S) ENOTSUP

int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);

int pthread_create(pthread_t *th, pthread_attr_t *attr,
    void *(* func)(void *), void *arg);

int pthread_join(pthread_t t, void **res);
int pthread_detach(pthread_t t);

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_gettype(pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_getpshared(pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t * a, int type);
int pthread_mutexattr_getprotocol(pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_getprioceiling(pthread_mutexattr_t *a, int * prio);
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *a, int prio);
int pthread_mutex_timedlock(pthread_mutex_t *m, struct timespec *ts);
#define _PTHREAD_BARRIER_FLAG (1<<30)
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_init(pthread_barrier_t *b, void *attr, int count);
int pthread_barrier_wait(pthread_barrier_t *b);

int pthread_barrierattr_init(void **attr);
int pthread_barrierattr_destroy(void **attr);
int pthread_barrierattr_setpshared(void **attr, int s);
int pthread_barrierattr_getpshared(void **attr, int *s);
typedef void (*_pthread_tls_dtor_t)(void*);
int pthread_key_create(pthread_key_t *key, _pthread_tls_dtor_t dest);
int pthread_key_delete(pthread_key_t key);

void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_spin_init(pthread_spinlock_t *l, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *l);
/* No-fair spinlock due to lack of knowledge of thread number */
int pthread_spin_lock(pthread_spinlock_t *l);
int pthread_spin_trylock(pthread_spinlock_t *l);
int pthread_spin_unlock(pthread_spinlock_t *l);

int pthread_cond_init(pthread_cond_t *c, pthread_condattr_t *a);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
    struct timespec *t);
int pthread_condattr_destroy(pthread_condattr_t *a);

#define pthread_condattr_getclock(A, C) ENOTSUP
#define pthread_condattr_setclock(A, C) ENOTSUP

int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_getpshared(pthread_condattr_t *a, int *s);
int pthread_condattr_setpshared(pthread_condattr_t *a, int s);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_getpshared(pthread_rwlockattr_t *a, int *s);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int s);

/* No fork() in windows - so ignore this */
#define pthread_atfork(F1,F2,F3) 0

/* Windows has rudimentary signals support */
#define pthread_kill(T, S) 0
#define pthread_sigmask(H, S1, S2) 0

#endif /* WIN_PTHREADS */

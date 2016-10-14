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
 * modification, are permitted provided that the following conditions are met:
 *
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

#include "watchman.h"
#include <windows.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/timeb.h>
#include <pthread.h>
#include <process.h>
#include <algorithm>

static volatile long _pthread_cancelling;

/* Will default to zero as needed */
static pthread_once_t _pthread_tls_once;
static DWORD _pthread_tls;

/* Note initializer is zero, so this works */
static pthread_rwlock_t _pthread_key_lock;
static pthread_key_t _pthread_key_max;
static pthread_key_t _pthread_key_sch;
static _pthread_tls_dtor_t *_pthread_key_dest;

static void _pthread_once_cleanup(void *arg) {
  auto o = (pthread_once_t*)arg;
  *o = 0;
}

/* Ensure the CriticalSection has been initialized */
static inline void ensure_mutex_init(pthread_mutex_t *m) {
   if (m->initialized) return;
   pthread_spin_lock(&m->initializer_spin_lock);
   if (!m->initialized) {
      InitializeCriticalSection(&m->cs);
      m->initialized = 1;
   }
   pthread_spin_unlock(&m->initializer_spin_lock);
}

static inline LPCRITICAL_SECTION pthread_mutex_cs_get(pthread_mutex_t *m) {
  ensure_mutex_init(m);
  return &m->cs;
}

int pthread_once(pthread_once_t *o, void (*func)(void))
{
  long state = *o;

  _ReadWriteBarrier();

  while (state != 1)
  {
    if (!state)
    {
      if (!_InterlockedCompareExchange(o, 2, 0))
      {
        /* Success */
        pthread_cleanup_push(_pthread_once_cleanup, o);
        func();
        pthread_cleanup_pop(0);

        /* Mark as done */
        *o = 1;

        return 0;
      }
    }

    YieldProcessor();

    _ReadWriteBarrier();

    state = *o;
  }

  /* Done */
  return 0;
}

int _pthread_once_raw(pthread_once_t *o, void (*func)(void))
{
  long state = *o;

  _ReadWriteBarrier();

  while (state != 1)
  {
    if (!state)
    {
      if (!_InterlockedCompareExchange(o, 2, 0))
      {
        /* Success */
        func();

        /* Mark as done */
        *o = 1;

        return 0;
      }
    }

    YieldProcessor();

    _ReadWriteBarrier();

    state = *o;
  }

  /* Done */
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
  EnterCriticalSection(pthread_mutex_cs_get(m));
  return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
  LeaveCriticalSection(pthread_mutex_cs_get(m));
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
  return TryEnterCriticalSection(pthread_mutex_cs_get(m)) ? 0 : EBUSY;
}

int pthread_mutex_init(pthread_mutex_t *m, pthread_mutexattr_t *a)
{
  (void) a;
  InitializeCriticalSection(&m->cs);
  m->initialized = TRUE;
  m->initializer_spin_lock = 0;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
  if (m->initialized) {
    DeleteCriticalSection(&m->cs);
    m->initialized = 0;
  }
  return 0;
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
  return t1 == t2;
}

int pthread_rwlock_init(pthread_rwlock_t *l, pthread_rwlockattr_t *a)
{
  (void) a;
  InitializeSRWLock(l);

  return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
  (void) *l;
  return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
  pthread_testcancel();
  AcquireSRWLockShared(l);

  return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
  pthread_testcancel();
  AcquireSRWLockExclusive(l);

  return 0;
}

void pthread_tls_init(void)
{
  _pthread_tls = TlsAlloc();

  /* Cannot continue if out of indexes */
  if (_pthread_tls == TLS_OUT_OF_INDEXES) abort();
}

void _pthread_cleanup_dest(pthread_t t)
{
  pthread_key_t i, j;

  for (j = 0; j < PTHREAD_DESTRUCTOR_ITERATIONS; j++)
  {
    int flag = 0;

    for (i = 0; i < t->keymax; i++)
    {
      void *val = t->keyval[i];

      if (val)
      {
        pthread_rwlock_rdlock(&_pthread_key_lock);
        if ((uintptr_t) _pthread_key_dest[i] > 1)
        {
          /* Call destructor */
          t->keyval[i] = NULL;
          _pthread_key_dest[i](val);
          flag = 1;
        }
        pthread_rwlock_unlock(&_pthread_key_lock);
      }
    }

    /* Nothing to do? */
    if (!flag) return;
  }
}

pthread_t pthread_self(void)
{
  struct _pthread_v *t = NULL;

  _pthread_once_raw(&_pthread_tls_once, pthread_tls_init);

  t = (_pthread_v*)TlsGetValue(_pthread_tls);
  /* Main thread? */
  if (t == NULL) {
    t = (_pthread_v*)calloc(1, sizeof(*t));

    // If cannot initialize main thread, then the only thing we can do is abort
    if (!t) {
      abort();
    }

    t->p_state = PTHREAD_DEFAULT_ATTR;
    t->h = GetCurrentThread();

    /* Save for later */
    TlsSetValue(_pthread_tls, t);

#if 0
    if (setjmp(t->jb))
    {
      /* Make sure we free ourselves if we are detached */
      if (!t->h) free(t);

      /* Time to die */
      _endthreadex(0);
    }
#endif
  }

  return t;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
  void *state = *(void **)l;

  if (state == (void *) 1)
  {
    /* Known to be an exclusive lock */
    ReleaseSRWLockExclusive(l);
  }
  else
  {
    /* A shared unlock will work */
    ReleaseSRWLockShared(l);
  }

  return 0;
}


int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
  /* Get the current state of the lock */
  void *state = *(void **) l;

  if (!state)
  {
    /* Unlocked to locked */
    if (!_InterlockedCompareExchangePointer((void **)l, (void *)0x11, NULL)) {
      return 0;
    }
    return EBUSY;
  }

  /* A single writer exists */
  if (state == (void *) 1) return EBUSY;

  /* Multiple writers exist? */
  if ((uintptr_t) state & 14) return EBUSY;

  if (_InterlockedCompareExchangePointer(
          (void **)l, (void *)((uintptr_t)state + 16), state) == state) {
    return 0;
  }

  return EBUSY;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
  /* Try to grab lock if it has no users */
  if (!_InterlockedCompareExchangePointer((void **)l, (void *)1, NULL)) {
    return 0;
  }

  return EBUSY;
}

unsigned long long _pthread_time_in_ms(void)
{
  struct __timeb64 tb;

  _ftime64(&tb);

  return tb.time * 1000 + tb.millitm;
}

unsigned long long _pthread_time_in_ms_from_timespec(const struct timespec *ts)
{
  unsigned long long t = ts->tv_sec * 1000;
  t += ts->tv_nsec / 1000000;

  return t;
}

unsigned long long _pthread_rel_time_in_ms(const struct timespec *ts)
{
  unsigned long long t1 = _pthread_time_in_ms_from_timespec(ts);
  unsigned long long t2 = _pthread_time_in_ms();

  /* Prevent underflow */
  if (t1 < t2) return 0;
  return t1 - t2;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *ts)
{
  unsigned long long ct = _pthread_time_in_ms();
  unsigned long long t = _pthread_time_in_ms_from_timespec(ts);

  pthread_testcancel();

  /* Use a busy-loop */
  while (1)
  {
    /* Try to grab lock */
    if (!pthread_rwlock_tryrdlock(l)) return 0;

    /* Get current time */
    ct = _pthread_time_in_ms();

    /* Have we waited long enough? */
    if (ct > t) return ETIMEDOUT;
  }
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *ts)
{
  unsigned long long ct = _pthread_time_in_ms();
  unsigned long long t = _pthread_time_in_ms_from_timespec(ts);

  pthread_testcancel();

  /* Use a busy-loop */
  while (1)
  {
    /* Try to grab lock */
    if (!pthread_rwlock_trywrlock(l)) return 0;

    /* Get current time */
    ct = _pthread_time_in_ms();

    /* Have we waited long enough? */
    if (ct > t) return ETIMEDOUT;
  }
}

int pthread_exit(void *res)
{
  pthread_t t = pthread_self();

  t->ret_arg = res;

  _pthread_cleanup_dest(t);

  longjmp(t->jb, 1);
}


void _pthread_invoke_cancel(void)
{
  _pthread_cleanup *pcup;

  _InterlockedDecrement(&_pthread_cancelling);

  /* Call cancel queue */
  for (pcup = pthread_self()->clean; pcup; pcup = pcup->next)
  {
    pcup->func(pcup->arg);
  }

  pthread_exit(PTHREAD_CANCELED);
}

void pthread_testcancel(void)
{
  if (_pthread_cancelling)
  {
    pthread_t t = pthread_self();

    if (t->cancelled && (t->p_state & PTHREAD_CANCEL_ENABLE))
    {
      _pthread_invoke_cancel();
    }
  }
}


int pthread_cancel(pthread_t t)
{
  if (t->p_state & PTHREAD_CANCEL_ASYNCHRONOUS)
  {
    /* Dangerous asynchronous cancelling */
    CONTEXT ctxt;

    /* Already done? */
    if (t->cancelled) return ESRCH;

    ctxt.ContextFlags = CONTEXT_CONTROL;

    SuspendThread(t->h);
    GetThreadContext(t->h, &ctxt);
#ifdef _M_X64
    ctxt.Rip = (uintptr_t) _pthread_invoke_cancel;
#else
    ctxt.Eip = (uintptr_t) _pthread_invoke_cancel;
#endif
    SetThreadContext(t->h, &ctxt);

    /* Also try deferred Cancelling */
    t->cancelled = 1;

    /* Notify everyone to look */
    _InterlockedIncrement(&_pthread_cancelling);

    ResumeThread(t->h);
  }
  else
  {
    /* Safe deferred Cancelling */
    t->cancelled = 1;

    /* Notify everyone to look */
    _InterlockedIncrement(&_pthread_cancelling);
  }

  return 0;
}

unsigned _pthread_get_state(pthread_attr_t *attr, unsigned flag)
{
  return attr->p_state & flag;
}

int _pthread_set_state(pthread_attr_t *attr, unsigned flag, unsigned val)
{
  if (~flag & val) return EINVAL;
  attr->p_state &= ~flag;
  attr->p_state |= val;

  return 0;
}

int pthread_attr_init(pthread_attr_t *attr)
{
  attr->p_state = PTHREAD_DEFAULT_ATTR;
  attr->stack = NULL;
  attr->s_size = 0;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
  unused_parameter(attr);
  /* No need to do anything */
  return 0;
}


int pthread_attr_setdetachstate(pthread_attr_t *a, int flag)
{
  return _pthread_set_state(a, PTHREAD_CREATE_DETACHED, flag);
}

int pthread_attr_getdetachstate(pthread_attr_t *a, int *flag)
{
  *flag = _pthread_get_state(a, PTHREAD_CREATE_DETACHED);
  return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *a, int flag)
{
  return _pthread_set_state(a, PTHREAD_INHERIT_SCHED, flag);
}

int pthread_attr_getinheritsched(pthread_attr_t *a, int *flag)
{
  *flag = _pthread_get_state(a, PTHREAD_INHERIT_SCHED);
  return 0;
}

int pthread_attr_setscope(pthread_attr_t *a, int flag)
{
  return _pthread_set_state(a, PTHREAD_SCOPE_SYSTEM, flag);
}

int pthread_attr_getscope(pthread_attr_t *a, int *flag)
{
  *flag = _pthread_get_state(a, PTHREAD_SCOPE_SYSTEM);
  return 0;
}

int pthread_attr_getstackaddr(pthread_attr_t *attr, void **stack)
{
  *stack = attr->stack;
  return 0;
}

int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stack)
{
  attr->stack = stack;
  return 0;
}

int pthread_attr_getstacksize(pthread_attr_t *attr, size_t *size)
{
  *size = attr->s_size;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
  attr->s_size = size;
  return 0;
}

int pthread_setcancelstate(int state, int *oldstate)
{
  pthread_t t = pthread_self();

  if ((state & PTHREAD_CANCEL_ENABLE) != state) return EINVAL;
  if (oldstate) *oldstate = t->p_state & PTHREAD_CANCEL_ENABLE;
  t->p_state &= ~PTHREAD_CANCEL_ENABLE;
  t->p_state |= state;

  return 0;
}

int pthread_setcanceltype(int type, int *oldtype)
{
  pthread_t t = pthread_self();

  if ((type & PTHREAD_CANCEL_ASYNCHRONOUS) != type) return EINVAL;
  if (oldtype) *oldtype = t->p_state & PTHREAD_CANCEL_ASYNCHRONOUS;
  t->p_state &= ~PTHREAD_CANCEL_ASYNCHRONOUS;
  t->p_state |= type;

  return 0;
}

static unsigned int __stdcall pthread_create_wrapper(void *args)
{
  auto tv = (_pthread_v*)args;

  _pthread_once_raw(&_pthread_tls_once, pthread_tls_init);

  TlsSetValue(_pthread_tls, tv);

  if (!setjmp(tv->jb))
  {
    /* Call function and save return value */
    tv->ret_arg = tv->func(tv->ret_arg);

    /* Clean up destructors */
    _pthread_cleanup_dest(tv);
  }

  /* If we exit too early, then we can race with create */
  while (tv->h == (HANDLE) -1)
  {
    YieldProcessor();
    _ReadWriteBarrier();
  }

  /* Make sure we free ourselves if we are detached */
  if (!tv->h) free(tv);

  return 0;
}

int pthread_create(pthread_t *th, pthread_attr_t *attr,
    void *(* func)(void *), void *arg)
{
  auto tv = (_pthread_v *)malloc(sizeof(struct _pthread_v));
  unsigned ssize = 0;

  if (!tv) return 1;

  *th = tv;

  /* Save data in pthread_t */
  tv->ret_arg = arg;
  tv->func = func;
  tv->clean = NULL;
  tv->cancelled = 0;
  tv->p_state = PTHREAD_DEFAULT_ATTR;
  tv->keymax = 0;
  tv->keyval = NULL;
  tv->h = (HANDLE) -1;

  if (attr)
  {
    tv->p_state = attr->p_state;
    ssize = (unsigned)attr->s_size;
  }

  /* Make sure tv->h has value of -1 */
  _ReadWriteBarrier();

  tv->h = (HANDLE) _beginthreadex(NULL, ssize, pthread_create_wrapper,
                      tv, 0, NULL);

  /* Failed */
  if (!tv->h) return 1;

  if (tv->p_state & PTHREAD_CREATE_DETACHED)
  {
    CloseHandle(tv->h);
    _ReadWriteBarrier();
    tv->h = 0;
  }

  return 0;
}

int pthread_join(pthread_t t, void **res)
{
  struct _pthread_v *tv = t;

  if (!tv) {
    return EINVAL;
  }

  pthread_testcancel();

  WaitForSingleObject(tv->h, INFINITE);
  CloseHandle(tv->h);

  /* Obtain return value */
  if (res) *res = tv->ret_arg;

  free(tv);

  return 0;
}

int pthread_detach(pthread_t t)
{
  struct _pthread_v *tv = t;

  /*
   * This can't race with thread exit because
   * our call would be undefined if called on a dead thread.
   */

  CloseHandle(tv->h);
  _ReadWriteBarrier();
  tv->h = 0;

  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
  *a = 0;
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
  (void) a;
  return 0;
}

int pthread_mutexattr_gettype(pthread_mutexattr_t *a, int *type)
{
  *type = *a & 3;

  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
  if ((unsigned) type > 3) return EINVAL;
  *a &= ~3;
  *a |= type;

  return 0;
}

int pthread_mutexattr_getpshared(pthread_mutexattr_t *a, int *type)
{
  *type = *a & 4;

  return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t * a, int type)
{
  if ((type & 4) != type) return EINVAL;

  *a &= ~4;
  *a |= type;

  return 0;
}

int pthread_mutexattr_getprotocol(pthread_mutexattr_t *a, int *type)
{
  *type = *a & (8 + 16);

  return 0;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int type)
{
  if ((type & (8 + 16)) != 8 + 16) return EINVAL;

  *a &= ~(8 + 16);
  *a |= type;

  return 0;
}

int pthread_mutexattr_getprioceiling(pthread_mutexattr_t *a, int * prio)
{
  *prio = *a / PTHREAD_PRIO_MULT;
  return 0;
}

int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *a, int prio)
{
  *a &= (PTHREAD_PRIO_MULT - 1);
  *a += prio * PTHREAD_PRIO_MULT;

  return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, struct timespec *ts)
{
  unsigned long long t, ct;

  /* Try to lock it without waiting */
  if (!pthread_mutex_trylock(m)) return 0;

  ct = _pthread_time_in_ms();
  t = _pthread_time_in_ms_from_timespec(ts);

  while (1)
  {
    /* Have we waited long enough? */
    if (ct >= t) return ETIMEDOUT;

    /* Wait on semaphore within critical section
     * We limit the wait time to 5 ms. For unknown reasons,
     * WaitForSingleObject fails to return in timely fashion
     * if we rely on the notification of m->LockSemaphore.
     * In addition, we could give a SpinCount
     * value on the critical section object and search for a sweet
     * spot granting a lock with no wait time on most systems.
     */
    DWORD timeout = (DWORD)(t - ct);
    timeout = std::min(timeout, DWORD(5));
    WaitForSingleObject(((CRITICAL_SECTION *)m)->LockSemaphore, timeout);

    /* Try to grab lock */
    if (!pthread_mutex_trylock(m)) return 0;

    /* Get current time */
    ct = _pthread_time_in_ms();
  }
}

#define _PTHREAD_BARRIER_FLAG (1<<30)

int pthread_barrier_destroy(pthread_barrier_t *b)
{
  EnterCriticalSection(&b->m);

  while (b->total > _PTHREAD_BARRIER_FLAG)
  {
    /* Wait until everyone exits the barrier */
    SleepConditionVariableCS(&b->cv, &b->m, INFINITE);
  }

  LeaveCriticalSection(&b->m);

  DeleteCriticalSection(&b->m);

  return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, void *attr, int count)
{
  /* Ignore attr */
  (void) attr;

  b->count = count;
  b->total = 0;

  InitializeCriticalSection(&b->m);
  InitializeConditionVariable(&b->cv);

  return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
  EnterCriticalSection(&b->m);

  while (b->total > _PTHREAD_BARRIER_FLAG)
  {
    /* Wait until everyone exits the barrier */
    SleepConditionVariableCS(&b->cv, &b->m, INFINITE);
  }

  /* Are we the first to enter? */
  if (b->total == _PTHREAD_BARRIER_FLAG) b->total = 0;

  b->total++;

  if (b->total == b->count)
  {
    b->total += _PTHREAD_BARRIER_FLAG - 1;
    WakeAllConditionVariable(&b->cv);

    LeaveCriticalSection(&b->m);

    return 1;
  }
  else
  {
    while (b->total < _PTHREAD_BARRIER_FLAG)
    {
      /* Wait until enough threads enter the barrier */
      SleepConditionVariableCS(&b->cv, &b->m, INFINITE);
    }

    b->total--;

    /* Get entering threads to wake up */
    if (b->total == _PTHREAD_BARRIER_FLAG) WakeAllConditionVariable(&b->cv);

    LeaveCriticalSection(&b->m);

    return 0;
  }
}

int pthread_barrierattr_init(void **attr)
{
  *attr = NULL;
  return 0;
}

int pthread_barrierattr_destroy(void **attr)
{
  /* Ignore attr */
  (void) attr;

  return 0;
}

int pthread_barrierattr_setpshared(void **attr, int s)
{
  *attr = (void *)(intptr_t)s;
  return 0;
}

int pthread_barrierattr_getpshared(void **attr, int *s)
{
  *s = (int) (size_t) *attr;

  return 0;
}

int pthread_key_create(pthread_key_t *key, void (* dest)(void *))
{
  pthread_key_t i;
  long nmax;
  void (**d)(void *);

  if (!key) return EINVAL;

  pthread_rwlock_wrlock(&_pthread_key_lock);

  for (i = _pthread_key_sch; i < _pthread_key_max; i++)
  {
    if (!_pthread_key_dest[i])
    {
      *key = i;
      if (dest)
      {
        _pthread_key_dest[i] = dest;
      }
      else
      {
        _pthread_key_dest[i] = (void(*)(void *))1;
      }
      pthread_rwlock_unlock(&_pthread_key_lock);

      return 0;
    }
  }

  for (i = 0; i < _pthread_key_sch; i++)
  {
    if (!_pthread_key_dest[i])
    {
      *key = i;
      if (dest)
      {
        _pthread_key_dest[i] = dest;
      }
      else
      {
        _pthread_key_dest[i] = (void(*)(void *))1;
      }
      pthread_rwlock_unlock(&_pthread_key_lock);

      return 0;
    }
  }

  if (!_pthread_key_max) _pthread_key_max = 1;
  if (_pthread_key_max == PTHREAD_KEYS_MAX)
  {
    pthread_rwlock_unlock(&_pthread_key_lock);

    return ENOMEM;
  }

  nmax = _pthread_key_max * 2;
  if (nmax > PTHREAD_KEYS_MAX) nmax = PTHREAD_KEYS_MAX;

  /* No spare room anywhere */
  d = (void (**)(void *))realloc(_pthread_key_dest, nmax * sizeof(*d));
  if (!d)
  {
    pthread_rwlock_unlock(&_pthread_key_lock);

    return ENOMEM;
  }

  /* Clear new region */
  memset((void *) &d[_pthread_key_max], 0,
      (nmax-_pthread_key_max)*sizeof(void *));

  /* Use new region */
  _pthread_key_dest = d;
  _pthread_key_sch = _pthread_key_max + 1;
  *key = _pthread_key_max;
  _pthread_key_max = nmax;

  if (dest)
  {
    _pthread_key_dest[*key] = dest;
  }
  else
  {
    _pthread_key_dest[*key] = (void(*)(void *))1;
  }

  pthread_rwlock_unlock(&_pthread_key_lock);

  return 0;
}

int pthread_key_delete(pthread_key_t key)
{
  if (key > _pthread_key_max) return EINVAL;
  if (!_pthread_key_dest) return EINVAL;

  pthread_rwlock_wrlock(&_pthread_key_lock);
  _pthread_key_dest[key] = NULL;

  /* Start next search from our location */
  if (_pthread_key_sch > key) _pthread_key_sch = key;

  pthread_rwlock_unlock(&_pthread_key_lock);

  return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
  pthread_t t = pthread_self();

  if (key >= t->keymax) return NULL;

  return t->keyval[key];

}

int pthread_setspecific(pthread_key_t key, const void *value)
{
  pthread_t t = pthread_self();

  if (key > t->keymax)
  {
    int keymax = (key + 1) * 2;
    void **kv = (void **)realloc(t->keyval, keymax * sizeof(void *));

    if (!kv) return ENOMEM;

    /* Clear new region */
    memset(&kv[t->keymax], 0, (keymax - t->keymax)*sizeof(void*));

    t->keyval = kv;
    t->keymax = keymax;
  }

  t->keyval[key] = (void *) value;

  return 0;
}


int pthread_spin_init(pthread_spinlock_t *l, int pshared)
{
  (void) pshared;

  *l = 0;
  return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *l)
{
  (void) l;
  return 0;
}

/* No-fair spinlock due to lack of knowledge of thread number */
int pthread_spin_lock(pthread_spinlock_t *l)
{
  if ( *l != 0 && *l != EBUSY )
  {
    w_log( W_LOG_FATAL, "Fatal error: spinlock value different from 0 or EBUSY! Smells like an uninitialized spinlock. Deadlock insight.\n");
  }

  while (_InterlockedExchange(l, EBUSY))
  {
    /* Don't lock the bus whilst waiting */
    while (*l)
    {
      YieldProcessor();

      /* Compiler barrier.  Prevent caching of *l */
      _ReadWriteBarrier();
    }
  }

  return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *l)
{
  return _InterlockedExchange(l, EBUSY);
}

int pthread_spin_unlock(pthread_spinlock_t *l)
{
  /* Compiler barrier.  The store below acts with release symmantics */
  _ReadWriteBarrier();

  *l = 0;

  return 0;
}

int pthread_cond_init(pthread_cond_t *c, pthread_condattr_t *a)
{
  (void) a;

  InitializeConditionVariable(c);
  return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
  WakeConditionVariable(c);
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
  WakeAllConditionVariable(c);
  return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
  pthread_testcancel();
  SleepConditionVariableCS(c, pthread_mutex_cs_get(m), INFINITE);
  return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
  (void) c;
  return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
    struct timespec *t)
{
  unsigned long long tm = _pthread_rel_time_in_ms(t);

  pthread_testcancel();

  if (!SleepConditionVariableCS(c, pthread_mutex_cs_get(m), (DWORD)tm)) {
    return map_win32_err(GetLastError());
  }

  /* We can have a spurious wakeup after the timeout */
  if (!_pthread_rel_time_in_ms(t)) return ETIMEDOUT;

  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a)
{
  (void) a;
  return 0;
}

int pthread_condattr_init(pthread_condattr_t *a)
{
  *a = 0;
  return 0;
}

int pthread_condattr_getpshared(pthread_condattr_t *a, int *s)
{
  *s = *a;
  return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *a, int s)
{
  *a = s;
  return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a)
{
  (void) a;
  return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *a)
{
  *a = 0;
  return 0;
}

int pthread_rwlockattr_getpshared(pthread_rwlockattr_t *a, int *s)
{
  *s = *a;
  return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int s)
{
  *a = s;
  return 0;
}

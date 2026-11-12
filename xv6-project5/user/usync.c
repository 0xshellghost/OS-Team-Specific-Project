#include "kernel/types.h"
#include "user/user.h"
#include "user/usync.h"

// The mutex is the three-state design from Drepper's "Futexes Are Tricky".
//   0  free
//   1  held, nobody is (or needs to be) asleep
//   2  held, and a sleeper may be waiting for the unlock
// Unlock only makes a system call when the state was 2.

void
fmutex_init(fmutex_t *m)
{
  m->state = 0;
}

void
fmutex_lock(fmutex_t *m)
{
  int c = 0;

  // fast path 0 -> 1. On failure c is updated to what was there.
  if (__atomic_compare_exchange_n(&m->state, &c, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return;

  // Contended. Mark the lock "has sleepers" (2) and sleep until we are the
  // one who sees it free. We leave it at 2 when we win, which costs one
  // spurious wake on our own unlock but can never lose a sleeper.
  if (c != 2)
    c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    if (futex_wait(&m->state, 2) < 0)
      return;       // killed; the kernel is about to end this process
    c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
  }
}

int
fmutex_trylock(fmutex_t *m)
{
  int c = 0;

  if (__atomic_compare_exchange_n(&m->state, &c, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  return -1;
}

void
fmutex_unlock(fmutex_t *m)
{
  // 1 -> 0 and nobody to wake, or it was 2 and we have to.
  if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE) != 1) {
    __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
    futex_wake(&m->state, 1);
  }
}

// Condition variable, sequence-number style. A waiter remembers seq, lets
// go of the mutex and sleeps *only if seq is still the same*. A signal that
// slips in after the unlock changes seq, so futex_wait() returns at once
// instead of sleeping through it. That is the no-lost-wakeup argument.
//
// The waiters count lets signal skip the system call when the queue is
// empty. That is safe as long as signalers change the predicate while they
// hold the mutex, which is the rule for condition variables anyway.

void
fcond_init(fcond_t *c)
{
  c->seq = 0;
  c->waiters = 0;
}

void
fcond_wait(fcond_t *c, fmutex_t *m)
{
  int seq;

  __atomic_fetch_add(&c->waiters, 1, __ATOMIC_SEQ_CST);
  seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);
  fmutex_unlock(m);
  futex_wait(&c->seq, seq);
  __atomic_fetch_sub(&c->waiters, 1, __ATOMIC_SEQ_CST);
  fmutex_lock(m);
}

void
fcond_signal(fcond_t *c)
{
  __atomic_fetch_add(&c->seq, 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST) > 0)
    futex_wake(&c->seq, 1);
}

void
fcond_broadcast(fcond_t *c)
{
  __atomic_fetch_add(&c->seq, 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST) > 0)
    futex_wake(&c->seq, -1);
}

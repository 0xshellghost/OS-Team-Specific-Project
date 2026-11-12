#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "waitq.h"

// Everything here is called with the lock that protects the queue held.
// That lock belongs to the caller (the semaphore, the condvar, the futex
// bucket); waitq.c never takes a lock of its own.

void
wq_init(struct waitq *q)
{
  q->head.next = q->head.prev = &q->head;
}

int
wq_empty(struct waitq *q)
{
  return q->head.next == &q->head;
}

// Put w at the tail of q.
void
wq_enqueue(struct waitq *q, struct waiter *w, uint64 key)
{
  w->key = key;
  w->signaled = 0;
  w->prev = q->head.prev;
  w->next = &q->head;
  q->head.prev->next = w;
  q->head.prev = w;
}

static void
wq_unlink(struct waiter *w)
{
  w->prev->next = w->next;
  w->next->prev = w->prev;
  w->prev = w->next = 0;
}

// Take w off its queue, mark it, and make its process runnable.
static void
wq_signal(struct waiter *w)
{
  wq_unlink(w);
  w->signaled = 1;
  wakeup(w);
}

// Block the calling process until another process signals w.
// w must already be on a queue, and lk (the lock protecting that queue)
// must be held. lk is released while we sleep and held again on return.
//
// Returns 0 once w was signaled, or -1 if the process was killed first
// (w is off the queue in both cases).
//
// This is sleep() written out by hand, with one addition. kill() only
// wakes a process that is already SLEEPING. If the kill lands after we
// let go of lk but before we are asleep, plain sleep() would go to sleep
// anyway and the process would stay blocked until the next real wakeup,
// possibly forever. So we look at p->killed with p->lock held, the same
// lock kill() takes, and refuse to sleep if the flag is already up.
int
wq_block(struct waiter *w, struct spinlock *lk)
{
  struct proc *p = myproc();
  int dying;

  while (!w->signaled) {
    acquire(&p->lock);
    release(lk);

    dying = p->killed;
    if (!dying) {
      p->chan = w;
      p->state = SLEEPING;
      sched();
      p->chan = 0;
      dying = p->killed;
    }

    release(&p->lock);
    acquire(lk);

    // A wakeup that raced with the kill wins: the waker already took us
    // off the queue and handed us whatever we were waiting for.
    if (dying && !w->signaled) {
      wq_unlink(w);
      return -1;
    }
  }
  return 0;
}

// Wake the first n waiters, in arrival order. n < 0 means everybody.
// Returns how many were woken.
int
wq_wake(struct waitq *q, int n)
{
  int woken = 0;

  while (n != 0 && !wq_empty(q)) {
    wq_signal(q->head.next);
    woken++;
    if (n > 0)
      n--;
  }
  return woken;
}

// Same, but only for waiters whose key matches (futexes share buckets).
int
wq_wake_key(struct waitq *q, uint64 key, int n)
{
  struct waiter *w, *next;
  int woken = 0;

  for (w = q->head.next; w != &q->head && n != 0; w = next) {
    next = w->next; // wq_signal() unlinks w
    if (w->key == key) {
      wq_signal(w);
      woken++;
      if (n > 0)
        n--;
    }
  }
  return woken;
}

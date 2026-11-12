// Kernel-managed counting semaphores and condition variables.
//
// Handles
// -------
// A semaphore or condvar is named by a small integer, like a file
// descriptor. Every process has a bitmask (p->semmask, p->condmask) of the
// handles it owns, and an object carries a count of how many processes
// hold it:
//
//   sem_init()      allocates an object and gives the caller a handle
//   fork()          the child gets the same handles, refs++ for each
//   sem_destroy()   drops the caller's handle, refs--
//   exit()          drops whatever handles are left
//
// An object is free again when its last holder lets go, so a crashing
// process can't leak table slots. Handles are checked against the mask,
// which also catches use after destroy and made-up numbers.
//
// Locking
// -------
//   semtab.lock / condtab.lock   slot allocation and the refs counts
//   ksem.lock / kcond.lock       value and wait queue of one object
//
// Lock order is cond->lock, then sem->lock (cond_wait releases the user's
// mutex while it holds the condvar's lock). Nothing ever goes the other
// way. p->lock comes last, as everywhere in xv6.
//
// Semaphores are strict FIFO. sem_post() with sleepers does not bump the
// counter, it hands the token straight to the longest waiter. Nobody can
// sneak in between the post and the wakeup, and a sleeper can't be starved
// by a process that keeps calling sem_wait() in a loop.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "waitq.h"

#define SEM_MAX 0x7fffffff

struct ksem {
  struct spinlock lock;
  int refs;          // holders, protected by semtab.lock
  int value;         // tokens available. Never > 0 while q is non-empty.
  struct waitq q;
};

struct kcond {
  struct spinlock lock;
  int refs;          // holders, protected by condtab.lock
  struct waitq q;
};

static struct {
  struct spinlock lock;
  struct ksem s[NSEM];
} semtab;

static struct {
  struct spinlock lock;
  struct kcond c[NCOND];
} condtab;

void futexinit(void); // futex.c

void
syncinit(void)
{
  int i;

  initlock(&semtab.lock, "semtab");
  for (i = 0; i < NSEM; i++) {
    initlock(&semtab.s[i].lock, "sem");
    wq_init(&semtab.s[i].q);
  }
  initlock(&condtab.lock, "condtab");
  for (i = 0; i < NCOND; i++) {
    initlock(&condtab.c[i].lock, "cond");
    wq_init(&condtab.c[i].q);
  }
  futexinit();
}

// ---------------------------------------------------------------------
// handle bookkeeping
// ---------------------------------------------------------------------

static struct ksem *
semlookup(int sd)
{
  struct proc *p = myproc();

  if (sd < 0 || sd >= NSEM || (p->semmask & (1UL << sd)) == 0)
    return 0;
  return &semtab.s[sd];
}

static struct kcond *
condlookup(int cd)
{
  struct proc *p = myproc();

  if (cd < 0 || cd >= NCOND || (p->condmask & (1UL << cd)) == 0)
    return 0;
  return &condtab.c[cd];
}

// fork(): np starts with all of p's handles.
void
semfork(struct proc *p, struct proc *np)
{
  int i;

  np->semmask = p->semmask;
  if (p->semmask == 0)
    return;
  acquire(&semtab.lock);
  for (i = 0; i < NSEM; i++)
    if (p->semmask & (1UL << i))
      semtab.s[i].refs++;
  release(&semtab.lock);
}

void
condfork(struct proc *p, struct proc *np)
{
  int i;

  np->condmask = p->condmask;
  if (p->condmask == 0)
    return;
  acquire(&condtab.lock);
  for (i = 0; i < NCOND; i++)
    if (p->condmask & (1UL << i))
      condtab.c[i].refs++;
  release(&condtab.lock);
}

// exit(): drop every handle p still has. A process that exits can't be
// asleep on any of them, so a count that reaches zero means an empty queue.
void
semexit(struct proc *p)
{
  int i;

  if (p->semmask == 0)
    return;
  acquire(&semtab.lock);
  for (i = 0; i < NSEM; i++)
    if (p->semmask & (1UL << i))
      semtab.s[i].refs--;
  release(&semtab.lock);
  p->semmask = 0;
}

void
condexit(struct proc *p)
{
  int i;

  if (p->condmask == 0)
    return;
  acquire(&condtab.lock);
  for (i = 0; i < NCOND; i++)
    if (p->condmask & (1UL << i))
      condtab.c[i].refs--;
  release(&condtab.lock);
  p->condmask = 0;
}

// ---------------------------------------------------------------------
// semaphore core. Both are also used by cond_wait().
// ---------------------------------------------------------------------

// P(). Returns 0 with a token in hand, or -1 if killed while waiting.
static int
semdown(struct ksem *s)
{
  struct waiter w;
  int r;

  acquire(&s->lock);
  if (s->value > 0) {
    s->value--;
    release(&s->lock);
    return 0;
  }
  // No token. Queue up; sem_post() will hand one directly to us.
  wq_enqueue(&s->q, &w, 0);
  r = wq_block(&w, &s->lock);
  release(&s->lock);
  return r;
}

// V(). Returns 0, or -1 if the counter would overflow.
static int
semup(struct ksem *s)
{
  acquire(&s->lock);
  if (wq_wake(&s->q, 1) == 0) {
    if (s->value == SEM_MAX) {
      release(&s->lock);
      return -1;
    }
    s->value++;
  }
  release(&s->lock);
  return 0;
}

// ---------------------------------------------------------------------
// semaphore system calls
// ---------------------------------------------------------------------

// int sem_init(int value) -> handle, or -1
uint64
sys_sem_init(void)
{
  struct proc *p = myproc();
  struct ksem *s;
  int value, i;

  argint(0, &value);
  if (value < 0)
    return -1;

  acquire(&semtab.lock);
  for (i = 0; i < NSEM; i++)
    if (semtab.s[i].refs == 0)
      break;
  if (i == NSEM) {
    release(&semtab.lock);
    return -1;
  }
  s = &semtab.s[i];
  s->refs = 1;
  s->value = value; // nobody else can see slot i until we publish the handle
  release(&semtab.lock);

  p->semmask |= 1UL << i;
  return i;
}

// int sem_wait(int sd) -> 0, or -1 (bad handle, or killed while waiting)
uint64
sys_sem_wait(void)
{
  struct ksem *s;
  int sd;

  argint(0, &sd);
  if ((s = semlookup(sd)) == 0)
    return -1;
  return semdown(s);
}

// int sem_trywait(int sd) -> 0 got a token, 1 would block, -1 bad handle
uint64
sys_sem_trywait(void)
{
  struct ksem *s;
  int sd, r;

  argint(0, &sd);
  if ((s = semlookup(sd)) == 0)
    return -1;

  acquire(&s->lock);
  if (s->value > 0) {
    s->value--;
    r = 0;
  } else {
    r = 1;
  }
  release(&s->lock);
  return r;
}

// int sem_post(int sd) -> 0, or -1 (bad handle, counter overflow)
uint64
sys_sem_post(void)
{
  struct ksem *s;
  int sd;

  argint(0, &sd);
  if ((s = semlookup(sd)) == 0)
    return -1;
  return semup(s);
}

// int sem_getvalue(int sd) -> current counter, or -1.
// Only a snapshot: it may be stale before the caller looks at it.
uint64
sys_sem_getvalue(void)
{
  struct ksem *s;
  int sd, v;

  argint(0, &sd);
  if ((s = semlookup(sd)) == 0)
    return -1;
  acquire(&s->lock);
  v = s->value;
  release(&s->lock);
  return v;
}

// int sem_destroy(int sd) -> 0, or -1
// Gives up this process's handle. The semaphore itself lives on for any
// other process that still holds it (a child, say).
uint64
sys_sem_destroy(void)
{
  struct proc *p = myproc();
  int sd;

  argint(0, &sd);
  if (semlookup(sd) == 0)
    return -1;

  p->semmask &= ~(1UL << sd);
  acquire(&semtab.lock);
  semtab.s[sd].refs--;
  release(&semtab.lock);
  return 0;
}

// ---------------------------------------------------------------------
// condition variables
//
// Mesa semantics: a signaled process has to get the mutex back before
// cond_wait() returns, and by then somebody else may have changed the
// state again, so callers re-test their predicate in a loop.
//
// The mutex is a semaphore used as a binary lock: value 0 means locked.
// Semaphores have no owner, so the only misuse we can catch is calling
// cond_wait() while the mutex is plainly not held (value > 0).
// ---------------------------------------------------------------------

// int cond_init(void) -> handle, or -1
uint64
sys_cond_init(void)
{
  struct proc *p = myproc();
  int i;

  acquire(&condtab.lock);
  for (i = 0; i < NCOND; i++)
    if (condtab.c[i].refs == 0)
      break;
  if (i == NCOND) {
    release(&condtab.lock);
    return -1;
  }
  condtab.c[i].refs = 1;
  release(&condtab.lock);

  p->condmask |= 1UL << i;
  return i;
}

// int cond_wait(int cd, int sd) -> 0, or -1
// Atomically release the mutex sd and sleep on cd; take sd again before
// returning. On -1 caused by a kill the mutex is not retaken, but a killed
// process never gets back to user code anyway.
uint64
sys_cond_wait(void)
{
  struct kcond *c;
  struct ksem *m;
  struct waiter w;
  int cd, sd, r;

  argint(0, &cd);
  argint(1, &sd);
  if ((c = condlookup(cd)) == 0 || (m = semlookup(sd)) == 0)
    return -1;

  acquire(&c->lock);
  acquire(&m->lock);
  if (m->value != 0) {
    // Mutex isn't locked, so the caller can't be holding it.
    release(&m->lock);
    release(&c->lock);
    return -1;
  }

  // Get on the condvar's queue *before* unlocking the mutex. A signaler
  // needs the mutex (or at least c->lock, which we hold until we are
  // asleep), so it can't slip a signal into the gap.
  wq_enqueue(&c->q, &w, 0);

  // Unlock the mutex: pass it to the first waiter, or raise the counter.
  if (wq_wake(&m->q, 1) == 0)
    m->value = 1;
  release(&m->lock);

  r = wq_block(&w, &c->lock);
  release(&c->lock);
  if (r < 0)
    return -1;

  return semdown(m);
}

// int cond_signal(int cd) -> 0, or -1. Wakes the longest waiter, if any.
uint64
sys_cond_signal(void)
{
  struct kcond *c;
  int cd;

  argint(0, &cd);
  if ((c = condlookup(cd)) == 0)
    return -1;
  acquire(&c->lock);
  wq_wake(&c->q, 1);
  release(&c->lock);
  return 0;
}

// int cond_broadcast(int cd) -> 0, or -1. Wakes everybody.
uint64
sys_cond_broadcast(void)
{
  struct kcond *c;
  int cd;

  argint(0, &cd);
  if ((c = condlookup(cd)) == 0)
    return -1;
  acquire(&c->lock);
  wq_wake(&c->q, -1);
  release(&c->lock);
  return 0;
}

// int cond_destroy(int cd) -> 0, or -1. Same idea as sem_destroy().
uint64
sys_cond_destroy(void)
{
  struct proc *p = myproc();
  int cd;

  argint(0, &cd);
  if (condlookup(cd) == 0)
    return -1;

  p->condmask &= ~(1UL << cd);
  acquire(&condtab.lock);
  condtab.c[cd].refs--;
  release(&condtab.lock);
  return 0;
}

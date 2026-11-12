// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

#define NLOCK_REG 512

static struct spinlock *all_locks[NLOCK_REG];
static int n_registered_locks = 0;

void
lock_profiler_register(struct spinlock *lk)
{
  for (int i = 0; i < n_registered_locks && i < NLOCK_REG; i++) {
    if (all_locks[i] == lk)
      return;
  }
  int idx = __atomic_fetch_add(&n_registered_locks, 1, __ATOMIC_SEQ_CST);
  if (idx < NLOCK_REG) {
    all_locks[idx] = lk;
  }
}

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
  lk->n_spins = 0;
  lk->n_acq = 0;
  lk->n_contended = 0;
  lock_profiler_register(lk);
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if (holding(lk))
    panic("acquire");

  // On RISC-V, __atomic_exchange_n turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  //
  // Passing __ATOMIC_ACQUIRE to __atomic_exchange_n tells
  // the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  uint64 spins = 0;
  while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0)
    spins++;

  // Record lock acquisition statistics. We now hold the lock.
  lk->n_acq++;
  if (spins > 0) {
    lk->n_contended++;
    lk->n_spins += spins;
  }

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if (!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Release the lock, equivalent to lk->locked = 0.
  //
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  //
  // On RISC-V, __atomic_store_n turns into a single atomic store:
  //   s1 = &lk->locked
  //   fence rw,w
  //   sw zero,0(s1)
  //
  // The __ATOMIC_RELEASE argument to __atomic_store_n tells the
  // the C compiler and the CPU to not move loads or stores past
  // this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  //
  // On RISC-V, this generates a fence instruction before the store:
  //   fence rw,w
  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  uint64 flags = rc_sstatus(SSTATUS_SIE);
  int old = !!(flags & SSTATUS_SIE);

  if (mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}

// Copy lock profiling statistics to user space.
// mode:
//   LOCKSTAT_AGGREGATE (0): aggregate statistics by lock name
//   LOCKSTAT_RESET     (1): reset counters to 0
//   LOCKSTAT_RAW       (2): raw per-spinlock statistics
int
lock_stat_copy(struct lock_stat *u_stats, int max, int mode)
{
  int total = n_registered_locks < NLOCK_REG ? n_registered_locks : NLOCK_REG;

  if (mode & LOCKSTAT_RESET) {
    for (int i = 0; i < total; i++) {
      struct spinlock *lk = all_locks[i];
      if (lk) {
        lk->n_spins = 0;
        lk->n_acq = 0;
        lk->n_contended = 0;
      }
    }
    if (u_stats == 0 || max <= 0)
      return 0;
  }

  if (u_stats == 0 || max <= 0)
    return -1;

  struct proc *p = myproc();

  if (mode & LOCKSTAT_RAW) {
    int count = 0;
    for (int i = 0; i < total && count < max; i++) {
      struct spinlock *lk = all_locks[i];
      if (!lk || !lk->name)
        continue;
      struct lock_stat s;
      memset(&s, 0, sizeof(s));
      safestrcpy(s.name, lk->name, sizeof(s.name));
      s.n_acq = lk->n_acq;
      s.n_contended = lk->n_contended;
      s.n_spins = lk->n_spins;

      if (copyout(p->pagetable, p->sz, (uint64)(u_stats + count), (char *)&s, sizeof(s)) < 0)
        return -1;
      count++;
    }
    return count;
  } else {
    #define MAX_LOCK_NAMES 64
    struct lock_stat agg[MAX_LOCK_NAMES];
    int nnames = 0;
    memset(agg, 0, sizeof(agg));

    for (int i = 0; i < total; i++) {
      struct spinlock *lk = all_locks[i];
      if (!lk || !lk->name)
        continue;

      int found = -1;
      for (int j = 0; j < nnames; j++) {
        if (strncmp(agg[j].name, lk->name, sizeof(agg[j].name)) == 0) {
          found = j;
          break;
        }
      }
      if (found < 0) {
        if (nnames < MAX_LOCK_NAMES) {
          found = nnames++;
          safestrcpy(agg[found].name, lk->name, sizeof(agg[found].name));
        } else {
          continue;
        }
      }
      agg[found].n_acq += lk->n_acq;
      agg[found].n_contended += lk->n_contended;
      agg[found].n_spins += lk->n_spins;
    }

    int count = nnames < max ? nnames : max;
    for (int k = 0; k < count; k++) {
      if (copyout(p->pagetable, p->sz, (uint64)(u_stats + k), (char *)&agg[k], sizeof(agg[k])) < 0)
        return -1;
    }
    return count;
  }
}


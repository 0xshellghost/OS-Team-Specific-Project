// futex_wait(addr, val) / futex_wake(addr, n)
//
// The kernel half of a futex: a place to sleep while a user-space word has
// a particular value, and a way to wake sleepers. All policy (what the word
// means, who owns the lock) stays in user space; see user/usync.c.
//
// Wait queues are keyed on the *physical* address of the word, not the
// virtual one. For pages mapped by shm_map() that is what makes the futex
// work between processes: parent and child see the word at the same virtual
// address, and so would any two unrelated private pages, but only shared
// pages have the same physical address.
//
// Queues live in a small hash table, NFUTEXQ buckets, one lock each, so
// unrelated futexes rarely contend. Each waiter sits in its bucket's list
// with its key; wake walks the (short) list and picks the matching ones in
// FIFO order.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "waitq.h"

static struct {
  struct spinlock lock;
  struct waitq q;
} bucket[NFUTEXQ];

void
futexinit(void)
{
  int i;

  for (i = 0; i < NFUTEXQ; i++) {
    initlock(&bucket[i].lock, "futex");
    wq_init(&bucket[i].q);
  }
}

#define BUCKET(key) (&bucket[((key) >> 2) % NFUTEXQ])

// int futex_wait(int *addr, int val)
//
// If *addr == val, sleep until futex_wake() on the same word picks us.
// The comparison and going to sleep happen under the bucket lock, and
// futex_wake() takes the same lock, so a wake can't fall between the two.
//
//   0   woken up
//   1   *addr != val, didn't sleep (the caller should look at the word again)
//  -1   bad address (null, unaligned, unmapped), or killed while waiting
uint64
sys_futex_wait(void)
{
  struct proc *p = myproc();
  uint64 uaddr, key;
  int val, r;
  struct waiter w;

  argaddr(0, &uaddr);
  argint(1, &val);
  if (uaddr % sizeof(int) != 0)
    return -1;
  if ((key = uva2pa(p->pagetable, p->sz, uaddr)) == 0)
    return -1;

  acquire(&BUCKET(key)->lock);
  // The kernel maps all RAM 1:1, so key is also a kernel pointer. A user
  // process on another CPU may be storing to this word right now, hence
  // the atomic load.
  if (__atomic_load_n((int *)key, __ATOMIC_ACQUIRE) != val) {
    release(&BUCKET(key)->lock);
    return 1;
  }
  wq_enqueue(&BUCKET(key)->q, &w, key);
  r = wq_block(&w, &BUCKET(key)->lock);
  release(&BUCKET(key)->lock);
  return r;
}

// int futex_wake(int *addr, int n)
// Wake at most n processes waiting on addr, oldest first (n < 0: all).
// Returns the number woken, or -1 for a bad address.
uint64
sys_futex_wake(void)
{
  struct proc *p = myproc();
  uint64 uaddr, key;
  int n, woken;

  argaddr(0, &uaddr);
  argint(1, &n);
  if (uaddr % sizeof(int) != 0)
    return -1;

  // Don't fault pages in just to wake nobody: a word whose page isn't
  // mapped yet (lazy heap) can't have any sleepers.
  key = walkaddr(p->pagetable, uaddr);
  if (key == 0)
    return uaddr < p->sz ? 0 : -1;
  key += uaddr & (PGSIZE - 1);

  acquire(&BUCKET(key)->lock);
  woken = wq_wake_key(&BUCKET(key)->q, key, n);
  release(&BUCKET(key)->lock);
  return woken;
}

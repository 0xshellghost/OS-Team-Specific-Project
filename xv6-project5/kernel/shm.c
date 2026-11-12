// shm_map(): memory that survives fork() as the *same* memory.
//
// xv6 processes normally share nothing, and there are no threads, so there
// is no way for two of them to look at one variable. A semaphore without
// shared data is not much use, so this is the smallest thing that fixes it:
// a window of pages, just below the trapframe, whose pages are shared by
// fork() instead of being copied. See shmcopy() in vm.c; the physical pages
// are reference counted in kalloc.c and go away with the last process that
// has them mapped (exit, or exec, which starts from a clean image).

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// void *shm_map(int npages)
// Map npages of zeroed shared memory. Returns its address, or -1.
// Successive calls stack downwards from SHMTOP; the heap is kept below.
uint64
sys_shm_map(void)
{
  struct proc *p = myproc();
  uint64 bytes, va;
  int npages;

  argint(0, &npages);
  if (npages <= 0 || npages > SHM_MAXPAGES)
    return -1;
  bytes = (uint64)npages * PGSIZE;
  if (p->shmsz + bytes > (uint64)SHM_MAXPAGES * PGSIZE)
    return -1;

  va = SHMTOP - p->shmsz - bytes;
  if (va < PGROUNDUP(p->sz)) // would collide with the heap
    return -1;

  if (shmmap(p->pagetable, va, npages) < 0)
    return -1;
  p->shmsz += bytes;
  return va;
}

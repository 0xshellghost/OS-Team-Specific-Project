// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// One reference count per physical page. A page that came out of kalloc()
// starts at 1; krefinc() adds a holder and kfree() only gives the page back
// once the last holder is gone. shm_map() pages rely on this: they sit in
// several page tables at once, and whoever exits last has to free them.
// Protected by kmem.lock.
#define PAIDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
static uchar refcnt[(PHYSTOP - KERNBASE) / PGSIZE];

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    refcnt[PAIDX(p)] = 1; // so that the kfree() below drops it to 0
    kfree(p);
  }
}

// Drop one reference to the page of physical memory pointed at by pa,
// which normally should have been returned by a call to kalloc().
// (The exception is when initializing the allocator; see kinit above.)
// The page goes back on the free list when the last reference is gone.
void
kfree(void *pa)
{
  struct run *r;
  int last;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  if (refcnt[PAIDX(pa)] == 0)
    panic("kfree: refcnt");
  last = (--refcnt[PAIDX(pa)] == 0);
  release(&kmem.lock);
  if (!last)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    refcnt[PAIDX(r)] = 1;
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

// Add a reference to a page that is already allocated.
void
krefinc(void *pa)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefinc");

  acquire(&kmem.lock);
  if (refcnt[PAIDX(pa)] == 0 || refcnt[PAIDX(pa)] == 255)
    panic("krefinc: bad count");
  refcnt[PAIDX(pa)]++;
  release(&kmem.lock);
}

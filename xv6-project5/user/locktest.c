// Unit tests for the kernel lock-contention profiler system call (lockstat).

#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "user/ptest.h"

static void
test_bad_args(void)
{
  CHECK(lockstat(0, 0, 0) == -1, "null pointer without reset flag returns -1");
  CHECK(lockstat((void *)-1, 10, 0) == -1, "invalid pointer returns -1");
  CHECK(lockstat((void *)0x1, 10, 0) == -1, "unaligned bad pointer returns -1");
}

static void
test_reset_and_read(void)
{
  struct lock_stat stats[32];

  CHECK(lockstat(0, 0, LOCKSTAT_RESET) == 0, "reset counters succeeds");

  int n = lockstat(stats, 32, LOCKSTAT_AGGREGATE);
  CHECK(n > 0, "aggregated read returns entries");

  int found_named = 0;
  for (int i = 0; i < n; i++) {
    if (strlen(stats[i].name) > 0)
      found_named++;
  }
  CHECK(found_named > 0, "found valid lock names");
}

static void
worker_contention(int id)
{
  // Allocate and free memory rapidly to hammer kmem.lock
  for (int i = 0; i < 200; i++) {
    char *p = sbrk(4096);
    if (p != SBRK_ERROR) {
      p[0] = (char)id;
      sbrk(-4096);
    }
  }
}

static void
test_contention_capture(void)
{
  struct lock_stat stats[32];

  // Reset counters
  CHECK(lockstat(0, 0, LOCKSTAT_RESET) == 0, "reset before workload");

  // Spawn concurrent workers to generate real lock contention on kmem
  for (int i = 0; i < 4; i++) {
    spawn(worker_contention, i);
  }
  reap();

  int n = lockstat(stats, 32, LOCKSTAT_AGGREGATE);
  CHECK(n > 0, "read stats after workload");

  uint64 total_acq = 0;
  for (int i = 0; i < n; i++) {
    total_acq += stats[i].n_acq;
  }
  CHECK(total_acq > 0, "acquisitions recorded during workload");

  // Raw per-spinlock mode check
  struct lock_stat raw_stats[64];
  int nraw = lockstat(raw_stats, 64, LOCKSTAT_RAW);
  CHECK(nraw > 0, "raw mode returns individual lock entries");
}

int
main(void)
{
  test_bad_args();
  test_reset_and_read();
  test_contention_capture();

  printf("locktest: PASS\n");
  exit(0);
}

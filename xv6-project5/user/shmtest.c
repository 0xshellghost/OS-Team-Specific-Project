// Tests for shm_map(): the shared pages that make fork-based
// synchronization possible, and the page reference counting under them.

#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "user/ptest.h"

static void
test_limits(void)
{
  CHECK(shm_map(0) == (void *)-1, "0 pages");
  CHECK(shm_map(-4) == (void *)-1, "negative pages");
  CHECK(shm_map(SHM_MAXPAGES + 1) == (void *)-1, "more than the cap");
}

static void
test_zeroed_and_stacking(void)
{
  char *a = shm_map(2);
  char *b = shm_map(1);
  CHECK(a != (char *)-1 && b != (char *)-1, "shm_map");
  CHECK(b + 4096 == a, "regions stack downwards without gaps");
  for (int i = 0; i < 2 * 4096; i += 97)
    CHECK(a[i] == 0, "fresh pages are zeroed");
  a[0] = 1;
  a[2 * 4096 - 1] = 2;   // both ends writable
  b[0] = 3;
}

static void
test_shared_with_child(void)
{
  int *c = shm_ints(1);
  c[0] = 10;
  int pid = fork();
  if (pid == 0) {
    c[0] += 5;           // visible to the parent
    c[1] = 99;
    exit(0);
  }
  wait(0);
  CHECK(c[0] == 15 && c[1] == 99, "parent sees the child's writes");
}

static void
test_heap_stays_private(void)
{
  int n = 100 * 1024;
  char *heap = malloc(n);
  int *c = shm_ints(1);
  CHECK(heap != 0, "malloc");
  memset(heap, 7, n);
  c[0] = 0;
  int pid = fork();
  if (pid == 0) {
    for (int i = 0; i < n; i += 511)
      if (heap[i] != 7)
        exit(1);          // child got a copy of the heap
    memset(heap, 8, n);   // ... which it may scribble on
    c[0] = 1;
    exit(0);
  }
  int st;
  wait(&st);
  CHECK(st == 0, "child saw the parent's heap contents");
  CHECK(c[0] == 1, "shared write arrived");
  for (int i = 0; i < n; i += 511)
    CHECK(heap[i] == 7, "child's heap writes did not leak into the parent");
  free(heap);
}

// the earlier tests already mapped 6 pages in this process, and the
// per-process cap counts them (children inherit them)
#define BIG (SHM_MAXPAGES - 8)

static void
test_no_leak_on_exit(void)
{
  // 600 * 56 pages is more than all of RAM. If exiting processes did not
  // give their shared pages back, kalloc would run dry long before the end.
  for (int i = 0; i < 600; i++) {
    int pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0) {
      char *m = shm_map(BIG);
      if (m == (char *)-1)
        exit(1);
      m[0] = 1;
      m[BIG * 4096 - 1] = 1;
      exit(0);
    }
    int st;
    wait(&st);
    CHECK(st == 0, "child could always map its pages");
  }
}

static void
test_no_leak_on_exec(void)
{
  // exec throws the shared window away; the pages must go with it
  char *args[] = { "echo", "x", 0 };
  for (int i = 0; i < 600; i++) {
    int pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0) {
      if (shm_map(BIG) == (void *)-1)
        exit(1);
      close(1);               // keep echo quiet
      exec("echo", args);
      exit(2);
    }
    int st;
    wait(&st);
    CHECK(st == 0, "map then exec");
  }
}

static void
test_many_sharers(void)
{
  // one page, eight processes holding it; it must survive until the last
  // of them has gone, whatever the order
  int *c = shm_ints(1);
  c[0] = 0;
  for (int i = 0; i < 8; i++) {
    int pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0) {
      nap(1 + i % 3);
      __atomic_fetch_add(&c[0], 1, __ATOMIC_SEQ_CST);
      exit(0);
    }
  }
  for (int i = 0; i < 8; i++)
    wait(0);
  CHECK(c[0] == 8, "all eight updates landed");
}

static void
test_fork_failure_paths(void)
{
  // fill the process table, fork keeps failing cleanly, and nothing leaks
  int pids[NPROC], n = 0;
  char *m = shm_map(4);
  CHECK(m != (char *)-1, "shm_map");
  for (;;) {
    int pid = fork();
    if (pid < 0)
      break;
    if (pid == 0) {
      for (;;)
        nap(100);
    }
    pids[n++] = pid;
  }
  CHECK(n > 10, "forked a good number");
  for (int i = 0; i < n; i++)
    kill(pids[i]);
  for (int i = 0; i < n; i++)
    wait(0);
  int pid = fork();
  if (pid == 0)
    exit(0);
  CHECK(pid > 0, "fork works again after the table drained");
  wait(0);
}

#define RUN(t) do { printf("  %s ... ", #t); t(); printf("ok\n"); } while (0)

int
main(void)
{
  printf("shmtest\n");
  RUN(test_limits);
  RUN(test_zeroed_and_stacking);
  RUN(test_shared_with_child);
  RUN(test_heap_stays_private);
  RUN(test_many_sharers);
  RUN(test_no_leak_on_exit);
  RUN(test_no_leak_on_exec);
  RUN(test_fork_failure_paths);
  printf("shmtest: PASS\n");
  exit(0);
}

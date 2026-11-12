// Unit tests for futex_wait/futex_wake and the futex-based user library.

#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "user/usync.h"
#include "user/ptest.h"

static int *w;   // futex words live in shared memory
static int priv; // a private word, to show that keys are physical

static void
sleeper(int id)
{
  int r = futex_wait(&w[0], 0);
  if (r != 0)
    exit(1);
  int slot = __atomic_fetch_add(&w[1], 1, __ATOMIC_SEQ_CST);
  w[2 + slot] = id;
}

static void
test_errors(void)
{
  w[0] = 5;
  CHECK(futex_wait(&w[0], 4) == 1, "value mismatch returns at once");
  CHECK(futex_wait((int *)((char *)&w[0] + 1), 5) == -1, "unaligned address");
  CHECK(futex_wait((int *)0x3fffff0000L, 0) == -1, "unmapped address");
  CHECK(futex_wait((int *)0x80000000L, 0) == -1, "kernel address");
  CHECK(futex_wake((int *)((char *)&w[0] + 2), 1) == -1, "unaligned wake");
  CHECK(futex_wake((int *)0x3fffff0000L, 1) == -1, "wake on unmapped address");
  CHECK(futex_wake(&w[0], 1) == 0, "wake with nobody asleep");
  CHECK(futex_wake(&w[0], -1) == 0, "wake all with nobody asleep");
}

static void
test_wake_counts(void)
{
  int i;
  w[0] = 0;
  w[1] = 0;
  for (i = 0; i < 5; i++) {
    spawn(sleeper, i);
    nap(2);
  }
  CHECK(futex_wake(&w[0], 1) == 1, "wake(1) wakes one");
  nap(2);
  CHECK(w[1] == 1, "one process ran");
  CHECK(futex_wake(&w[0], 2) == 2, "wake(2) wakes two");
  nap(2);
  CHECK(w[1] == 3, "three have run");
  CHECK(futex_wake(&w[0], 0) == 0, "wake(0) wakes nobody");
  CHECK(futex_wake(&w[0], 100) == 2, "wake(100) wakes the rest, and reports 2");
  CHECK(reap() == 0, "sleepers finished");
  for (i = 0; i < 5; i++)
    CHECK(w[2 + i] == i, "woken oldest first");
  CHECK(futex_wake(&w[0], 1) == 0, "queue is empty now");
}

static void
test_other_word(void)
{
  // different word in the same page: must not be woken by a wake on its neighbour
  w[0] = 0;
  w[1] = 0;
  int pid = spawn(sleeper, 7);
  nap(2);
  CHECK(futex_wake(&w[16], 1) == 0, "wake on a different word hits nothing");
  nap(1);
  CHECK(w[1] == 0, "sleeper is still asleep");
  CHECK(futex_wake(&w[0], 1) == 1, "wake on its own word works");
  CHECK(reap() == 0, "sleeper finished");
  (void)pid;
}

static void
private_sleeper(int arg)
{
  priv = 0;
  futex_wait(&priv, 0);
  exit(5); // reached only if somebody wakes us
}

static void
test_private_is_private(void)
{
  // Child sleeps on &priv in its own address space. We have a variable at
  // the same virtual address, but it's a different physical page.
  int pid = spawn(private_sleeper, 0);
  nap(2);
  CHECK(futex_wake(&priv, 10) == 0, "same VA, other process: not the same futex");
  CHECK(kill_and_wait(pid) == -1, "sleeper was still asleep, and dies on kill");
}

static void
test_kill_sleeper(void)
{
  w[0] = 0;
  w[1] = 0;
  int pid = spawn(sleeper, 1);
  nap(2);
  CHECK(kill_and_wait(pid) == -1, "killed");
  CHECK(futex_wake(&w[0], 10) == 0, "dead sleeper is not on the queue any more");
}

static void
parent_wakes_child_side(int arg)
{
  // child sets the word and wakes the parent
  nap(2);
  __atomic_store_n(&w[0], 1, __ATOMIC_SEQ_CST);
  futex_wake(&w[0], 1);
}

static void
test_cross_process(void)
{
  w[0] = 0;
  spawn(parent_wakes_child_side, 0);
  while (__atomic_load_n(&w[0], __ATOMIC_SEQ_CST) == 0)
    futex_wait(&w[0], 0);
  CHECK(reap() == 0, "child finished");
}

static fmutex_t *fm;
static int *count;

static void
fm_worker(int arg)
{
  for (int i = 0; i < 500; i++) {
    fmutex_lock(fm);
    int t = *count;
    spin(10);
    *count = t + 1;
    fmutex_unlock(fm);
  }
}

static void
test_fmutex(void)
{
  int i;
  *count = 0;
  for (i = 0; i < 4; i++)
    spawn(fm_worker, 0);
  watchdog(300);
  CHECK(reap() == 0, "workers finished");
  CHECK(*count == 2000, "futex mutex: no lost updates");
  CHECK(fmutex_trylock(fm) == 0, "trylock on a free mutex");
  CHECK(fmutex_trylock(fm) == -1, "trylock on a held mutex");
  fmutex_unlock(fm);
}

static fcond_t *fc;

static void
fc_producer(int arg)
{
  for (int i = 1; i <= 400; i++) {
    fmutex_lock(fm);
    while (count[1] != 0)       // one-slot mailbox
      fcond_wait(fc, fm);
    count[1] = i;
    fcond_broadcast(fc);
    fmutex_unlock(fm);
  }
}

static void
fc_consumer(int arg)
{
  long sum = 0;
  for (int i = 1; i <= 400; i++) {
    fmutex_lock(fm);
    while (count[1] == 0)
      fcond_wait(fc, fm);
    sum += count[1];
    count[1] = 0;
    fcond_broadcast(fc);
    fmutex_unlock(fm);
  }
  count[2] = (int)sum;
}

static void
test_fcond(void)
{
  count[1] = 0;
  count[2] = 0;
  spawn(fc_producer, 0);
  spawn(fc_consumer, 0);
  watchdog(300);
  CHECK(reap() == 0, "both finished");
  CHECK(count[2] == 400 * 401 / 2, "mailbox handed over every value exactly once");
}

#define RUN(t) do { printf("  %s ... ", #t); t(); printf("ok\n"); } while (0)

int
main(void)
{
  char *mem = (char *)shm_ints(1);
  w = (int *)mem;                              // first half: futex words
  fm = (fmutex_t *)(mem + 2048);
  fc = (fcond_t *)(mem + 2048 + 64);
  count = (int *)(mem + 2048 + 128);
  fmutex_init(fm);
  fcond_init(fc);

  printf("futextest\n");
  RUN(test_errors);
  RUN(test_wake_counts);
  RUN(test_other_word);
  RUN(test_private_is_private);
  RUN(test_kill_sleeper);
  RUN(test_cross_process);
  RUN(test_fmutex);
  RUN(test_fcond);
  printf("futextest: PASS\n");
  exit(0);
}

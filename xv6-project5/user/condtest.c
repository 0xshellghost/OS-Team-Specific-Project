// Unit tests for the kernel condition variables.
// The mutex is a binary semaphore, as cond_wait() expects.

#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "user/ptest.h"

static int *sh;   // sh[0] woken, sh[1] log position, sh[2..] log, sh[40] ping-pong turn

static int mu, cv; // handles, inherited by the children

static void
waiter(int id)
{
  if (sem_wait(mu) < 0)
    exit(1);
  // predicate: sh[3] != 0, set by the parent. Loop because of Mesa semantics.
  while (sh[3] == 0)
    if (cond_wait(cv, mu) < 0)
      exit(1);
  int slot = __atomic_fetch_add(&sh[1], 1, __ATOMIC_SEQ_CST);
  sh[8 + slot] = id;
  __atomic_fetch_add(&sh[0], 1, __ATOMIC_SEQ_CST);
  sem_post(mu);
}

// Variant where a signal consumes the predicate, so that exactly one
// waiter can proceed per signal.
static void
ticket_waiter(int id)
{
  if (sem_wait(mu) < 0)
    exit(1);
  while (sh[3] == 0)
    if (cond_wait(cv, mu) < 0)
      exit(1);
  sh[3]--;
  int slot = __atomic_fetch_add(&sh[1], 1, __ATOMIC_SEQ_CST);
  sh[8 + slot] = id;
  __atomic_fetch_add(&sh[0], 1, __ATOMIC_SEQ_CST);
  sem_post(mu);
}

static void
reset(void)
{
  sh[0] = sh[1] = sh[3] = 0;
}

static void
test_misuse(void)
{
  int m = sem_init(1), c = cond_init();
  CHECK(c >= 0 && m >= 0, "init");
  CHECK(cond_wait(c, m) == -1, "cond_wait with an unlocked mutex is refused");
  CHECK(cond_wait(-1, m) == -1, "bad condvar handle");
  CHECK(cond_wait(c, 99) == -1, "bad mutex handle");
  CHECK(cond_signal(NCOND) == -1, "signal on bad handle");
  CHECK(cond_broadcast(-3) == -1, "broadcast on bad handle");
  CHECK(cond_signal(c) == 0, "signal with nobody waiting is fine");
  CHECK(cond_destroy(c) == 0, "destroy");
  CHECK(cond_destroy(c) == -1, "destroy twice");
  CHECK(cond_signal(c) == -1, "signal after destroy");
  sem_destroy(m);
}

static void
test_table_full(void)
{
  int h[NCOND + 1], n = 0, i;
  while (n <= NCOND && (h[n] = cond_init()) >= 0)
    n++;
  CHECK(n == NCOND, "exactly NCOND condvars");
  for (i = 0; i < n; i++)
    cond_destroy(h[i]);
  CHECK((h[0] = cond_init()) >= 0, "slot free again");
  cond_destroy(h[0]);
}

static void
test_signal_one(void)
{
  int i;
  mu = sem_init(1);
  cv = cond_init();
  reset();
  for (i = 0; i < 3; i++) {
    spawn(ticket_waiter, i);
    nap(2); // arrival order 0, 1, 2
  }
  CHECK(sh[0] == 0, "nobody got through yet");

  for (i = 1; i <= 3; i++) {
    sem_wait(mu);
    sh[3] = 1;                  // one ticket
    cond_signal(cv);
    sem_post(mu);
    nap(2);
    CHECK(sh[0] == i, "each signal releases exactly one waiter");
  }
  CHECK(reap() == 0, "waiters finished");
  for (i = 0; i < 3; i++)
    CHECK(sh[8 + i] == i, "signals wake the oldest waiter first");
  sem_destroy(mu);
  cond_destroy(cv);
}

static void
test_broadcast(void)
{
  int i;
  mu = sem_init(1);
  cv = cond_init();
  reset();
  for (i = 0; i < 5; i++)
    spawn(waiter, i);
  nap(4);
  CHECK(sh[0] == 0, "all five are asleep");
  sem_wait(mu);
  sh[3] = 1;
  cond_broadcast(cv);
  sem_post(mu);
  watchdog(100);
  CHECK(reap() == 0, "waiters finished");
  CHECK(sh[0] == 5, "broadcast woke everybody");
  CHECK(sem_getvalue(mu) == 1, "mutex is free at the end");
  sem_destroy(mu);
  cond_destroy(cv);
}

static void
test_signal_not_remembered(void)
{
  mu = sem_init(1);
  cv = cond_init();
  reset();
  // a signal nobody hears is gone; it must not wake a later waiter
  cond_signal(cv);
  spawn(ticket_waiter, 0);
  nap(3);
  CHECK(sh[0] == 0, "late waiter still blocked");
  sem_wait(mu);
  sh[3] = 1;
  cond_signal(cv);
  sem_post(mu);
  watchdog(100);
  CHECK(reap() == 0, "waiter finished");
  CHECK(sh[0] == 1, "and the real signal woke it");
  sem_destroy(mu);
  cond_destroy(cv);
}

static void
test_kill_waiter(void)
{
  mu = sem_init(1);
  cv = cond_init();
  reset();
  int pid = spawn(waiter, 0);
  nap(3);
  CHECK(sem_getvalue(mu) == 1, "cond_wait gave the mutex back");
  CHECK(kill_and_wait(pid) == -1, "killed");
  // dead waiter must be off the queue: a broadcast must not trip on it
  CHECK(cond_broadcast(cv) == 0, "broadcast after the kill");
  CHECK(sem_trywait(mu) == 0, "mutex is still usable");
  sem_post(mu);
  sem_destroy(mu);
  cond_destroy(cv);
}

// two processes take turns through one condvar; any lost wakeup hangs
static void
pinger(int me)
{
  for (int i = 0; i < 300; i++) {
    sem_wait(mu);
    while (sh[40] != me)
      cond_wait(cv, mu);
    sh[41]++;
    sh[40] = 1 - me;
    cond_broadcast(cv);
    sem_post(mu);
  }
}

static void
test_pingpong(void)
{
  mu = sem_init(1);
  cv = cond_init();
  sh[40] = 0;
  sh[41] = 0;
  spawn(pinger, 0);
  spawn(pinger, 1);
  watchdog(300);
  CHECK(reap() == 0, "both finished");
  CHECK(sh[41] == 600, "every turn was taken");
  sem_destroy(mu);
  cond_destroy(cv);
}

#define RUN(t) do { printf("  %s ... ", #t); t(); printf("ok\n"); } while (0)

int
main(void)
{
  sh = shm_ints(1);
  printf("condtest\n");
  RUN(test_misuse);
  RUN(test_table_full);
  RUN(test_signal_one);
  RUN(test_broadcast);
  RUN(test_signal_not_remembered);
  RUN(test_kill_waiter);
  RUN(test_pingpong);
  printf("condtest: PASS\n");
  exit(0);
}

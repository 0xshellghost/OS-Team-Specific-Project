// Unit tests for the semaphore system calls.

#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "user/ptest.h"

static int *shared; // shared[0..]: scratch cells for the workers

// --- individual tests -------------------------------------------------

static void
test_basic(void)
{
  int s = sem_init(2);
  CHECK(s >= 0, "sem_init");
  CHECK(sem_getvalue(s) == 2, "initial value");
  CHECK(sem_wait(s) == 0 && sem_wait(s) == 0, "two waits succeed");
  CHECK(sem_getvalue(s) == 0, "value is 0 after two waits");
  CHECK(sem_trywait(s) == 1, "trywait on empty reports would-block");
  CHECK(sem_post(s) == 0, "post");
  CHECK(sem_getvalue(s) == 1, "value is 1 after post");
  CHECK(sem_trywait(s) == 0, "trywait takes the token");
  CHECK(sem_destroy(s) == 0, "destroy");
  CHECK(sem_destroy(s) == -1, "second destroy fails");
  CHECK(sem_wait(s) == -1, "wait on destroyed handle fails");
  CHECK(sem_post(s) == -1, "post on destroyed handle fails");
}

static void
test_bad_handles(void)
{
  CHECK(sem_init(-1) == -1, "negative initial value rejected");
  CHECK(sem_wait(-1) == -1, "wait(-1)");
  CHECK(sem_wait(NSEM) == -1, "wait(NSEM)");
  CHECK(sem_post(100000) == -1, "post(100000)");
  CHECK(sem_trywait(-5) == -1, "trywait(-5)");
  CHECK(sem_destroy(NSEM + 3) == -1, "destroy out of range");
  // a handle that was destroyed is dead for good
  int s = sem_init(0);
  CHECK(s >= 0, "sem_init");
  CHECK(sem_destroy(s) == 0, "destroy");
  CHECK(sem_getvalue(s) == -1, "stale handle");
}

static void
test_overflow(void)
{
  int s = sem_init(0x7fffffff);
  CHECK(s >= 0, "init INT_MAX");
  CHECK(sem_post(s) == -1, "post at INT_MAX fails");
  CHECK(sem_wait(s) == 0, "wait");
  CHECK(sem_post(s) == 0, "post fits again");
  sem_destroy(s);
}

static void
test_table_full(void)
{
  int h[NSEM + 1], n = 0, i;

  while (n <= NSEM && (h[n] = sem_init(1)) >= 0)
    n++;
  CHECK(n == NSEM, "exactly NSEM semaphores can exist");
  CHECK(sem_init(1) == -1, "table full");
  for (i = 0; i < n; i++)
    CHECK(sem_destroy(h[i]) == 0, "destroy all");
  CHECK((h[0] = sem_init(1)) >= 0, "slot free again");
  sem_destroy(h[0]);
}

static void
leaky_child(int arg)
{
  // grabs every slot and exits without destroying any of them
  while (sem_init(0) >= 0)
    ;
}

static void
test_exit_releases(void)
{
  spawn(leaky_child, 0);
  CHECK(reap() == 0, "child ran");
  // if the child's handles had leaked, this would come up short
  int h[NSEM + 1], n = 0, i;
  while (n <= NSEM && (h[n] = sem_init(0)) >= 0)
    n++;
  CHECK(n == NSEM, "dead process released its semaphores");
  for (i = 0; i < n; i++)
    sem_destroy(h[i]);
}

static void
blocker(int s)
{
  if (sem_wait(s) < 0)
    exit(1);
  shared[0] = 1;
}

static void
test_blocking(void)
{
  int s = sem_init(0);
  shared[0] = 0;
  spawn(blocker, s);
  nap(3);
  CHECK(shared[0] == 0, "waiter is still blocked");
  CHECK(sem_getvalue(s) == 0, "no token while a waiter sleeps");
  CHECK(sem_post(s) == 0, "post");
  CHECK(reap() == 0, "waiter finished");
  CHECK(shared[0] == 1, "waiter ran after the post");
  CHECK(sem_getvalue(s) == 0, "token went straight to the waiter");
  sem_destroy(s);
}

static void
fifo_waiter(int arg)
{
  int s = arg >> 8, id = arg & 0xff;
  if (sem_wait(s) < 0)
    exit(1);
  int slot = __atomic_fetch_add(&shared[1], 1, __ATOMIC_SEQ_CST);
  shared[2 + slot] = id;
}

static void
test_fifo(void)
{
  int s = sem_init(0), i;
  shared[1] = 0;
  for (i = 0; i < 6; i++) {
    spawn(fifo_waiter, (s << 8) | i);
    nap(2); // let it block before the next one arrives
  }
  for (i = 0; i < 6; i++) {
    CHECK(sem_post(s) == 0, "post");
    nap(2); // let the woken one log itself
  }
  CHECK(reap() == 0, "waiters finished");
  for (i = 0; i < 6; i++)
    CHECK(shared[2 + i] == i, "wakeups come in arrival order");
  sem_destroy(s);
}

static void
test_kill_waiter(void)
{
  int s = sem_init(0);
  int pid = spawn(blocker, s);
  nap(3);
  int st = kill_and_wait(pid);
  CHECK(st == -1, "killed waiter exits with -1");
  // The dead process must not still be on the queue, or this token
  // would be handed to a ghost and lost.
  CHECK(sem_post(s) == 0, "post");
  CHECK(sem_getvalue(s) == 1, "token was not eaten by the dead waiter");
  CHECK(sem_trywait(s) == 0, "token is there");
  sem_destroy(s);
}

static void
user_of_inherited(int s)
{
  // parent has already destroyed its own handle
  if (sem_wait(s) != 0)
    exit(1);
  if (sem_getvalue(s) != 0)
    exit(1);
}

static void
test_inherit(void)
{
  int s = sem_init(1);
  spawn(user_of_inherited, s);
  CHECK(sem_destroy(s) == 0, "parent drops its handle");
  CHECK(sem_post(s) == -1, "parent can no longer use it");
  CHECK(reap() == 0, "child still has a working semaphore");
}

static void
counter_worker(int arg)
{
  int s = arg;
  for (int i = 0; i < 400; i++) {
    sem_wait(s);
    int t = shared[0];
    spin(20);           // wide window for a race, if there were one
    shared[0] = t + 1;
    sem_post(s);
  }
}

static void
test_mutual_exclusion(void)
{
  int s = sem_init(1), i;
  shared[0] = 0;
  for (i = 0; i < 4; i++)
    spawn(counter_worker, s);
  watchdog(300);
  CHECK(reap() == 0, "workers finished");
  CHECK(shared[0] == 1600, "no lost updates under a binary semaphore");
  sem_destroy(s);
}

static void
test_counting(void)
{
  // a semaphore with N tokens lets exactly N processes through at once
  int s = sem_init(3), i;
  shared[0] = 0; // inside right now
  shared[1] = 0; // most that were ever inside together
  for (i = 0; i < 6; i++) {
    int pid = fork();
    if (pid == 0) {
      for (int k = 0; k < 4; k++) {
        sem_wait(s);
        int in = __atomic_add_fetch(&shared[0], 1, __ATOMIC_SEQ_CST);
        int m = shared[1];
        while (in > m && !__atomic_compare_exchange_n(&shared[1], &m, in, 0,
               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
          ;
        nap(1);   // hold the token across a timer tick so the others pile up
        __atomic_sub_fetch(&shared[0], 1, __ATOMIC_SEQ_CST);
        sem_post(s);
      }
      exit(0);
    }
    kids[nkids++] = pid;
  }
  watchdog(300);
  CHECK(reap() == 0, "workers finished");
  CHECK(shared[1] == 3, "exactly 3 were inside together, never more");
  sem_destroy(s);
}

// --- driver -----------------------------------------------------------

#define RUN(t) do { printf("  %s ... ", #t); t(); printf("ok\n"); } while (0)

int
main(void)
{
  shared = shm_ints(1);
  printf("semtest\n");
  RUN(test_basic);
  RUN(test_bad_handles);
  RUN(test_overflow);
  RUN(test_table_full);
  RUN(test_exit_releases);
  RUN(test_blocking);
  RUN(test_fifo);
  RUN(test_kill_waiter);
  RUN(test_inherit);
  RUN(test_mutual_exclusion);
  RUN(test_counting);
  printf("semtest: PASS\n");
  exit(0);
}

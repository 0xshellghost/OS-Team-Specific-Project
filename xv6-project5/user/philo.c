// Dining philosophers: five of them, five chopsticks, each philosopher
// needs the two next to him.
//
//   philo                      the four correct solutions, one after another
//   philo MODE [-v]            one solution; -v prints who eats when
//
//   MODE:
//     naive       pick up left, then right. Deadlocks, on purpose: the
//                 philosophers pause between the two grabs so that it
//                 happens every time. The test passes when it is detected.
//     order       resource hierarchy: always take the lower-numbered
//                 chopstick first, so no cycle of waiting can form
//     room        a "waiter" semaphore lets at most four to the table, so
//                 somebody can always get both chopsticks
//     tanenbaum   the textbook state-array solution, one semaphore per
//                 philosopher to sleep on, one mutex for the state
//     monitor     the same idea written as a monitor: a mutex plus one
//                 kernel condition variable per philosopher
//
// A shared array records who is eating at the moment. Two neighbours eating
// together counts as a violation and fails the run.

#include "kernel/types.h"
#include "user/user.h"
#include "user/ptest.h"

#define N      5
#define MEALS  20
#define LEFT(i)  (((i) + N - 1) % N)
#define RIGHT(i) (((i) + 1) % N)

enum { THINKING, HUNGRY, EATING };
enum { NAIVE, ORDER, ROOM, TANENBAUM, MONITOR };

struct table {
  int eating[N];
  int meals[N];
  int state[N];
  int violations;
  int done;
};

static struct table *T;
static int mode, verbose;
static int chop[N];          // one semaphore per chopstick
static int room_s;           // seats, N-1 of them
static int mutex_s;          // guards state[] (tanenbaum, monitor)
static int self_s[N];        // tanenbaum: philosopher i sleeps here
static int self_c[N];        // monitor: philosopher i waits on this condvar
static int out_s;            // keeps -v lines from mixing

static void
say(int i, const char *what)
{
  if (!verbose)
    return;
  sem_wait(out_s);
  printf("philosopher %d %s\n", i, what);
  sem_post(out_s);
}

static void
eat(int i, uint *seed)
{
  __atomic_store_n(&T->eating[i], 1, __ATOMIC_SEQ_CST);
  // Set my own flag first, then look: if two neighbours ever eat together
  // at least one of them is sure to see the other.
  if (__atomic_load_n(&T->eating[LEFT(i)], __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&T->eating[RIGHT(i)], __ATOMIC_SEQ_CST))
    __atomic_fetch_add(&T->violations, 1, __ATOMIC_SEQ_CST);
  say(i, "is eating");
  spin(rnd(seed) % 500);
  T->meals[i]++;
  __atomic_store_n(&T->eating[i], 0, __ATOMIC_SEQ_CST);
}

// tanenbaum / monitor helper, called with mutex_s held
static void
test(int i)
{
  if (T->state[i] == HUNGRY && T->state[LEFT(i)] != EATING &&
      T->state[RIGHT(i)] != EATING) {
    T->state[i] = EATING;
    sem_post(self_s[i]);
  }
}

static void
pickup(int i)
{
  int a, b;

  switch (mode) {
  case NAIVE:
    sem_wait(chop[i]);
    nap(1);                       // everybody now holds their left one
    sem_wait(chop[RIGHT(i)]);
    break;
  case ORDER:
    a = i < RIGHT(i) ? i : RIGHT(i);
    b = i < RIGHT(i) ? RIGHT(i) : i;
    sem_wait(chop[a]);
    sem_wait(chop[b]);
    break;
  case ROOM:
    sem_wait(room_s);
    sem_wait(chop[i]);
    sem_wait(chop[RIGHT(i)]);
    break;
  case TANENBAUM:
    sem_wait(mutex_s);
    T->state[i] = HUNGRY;
    test(i);
    sem_post(mutex_s);
    sem_wait(self_s[i]);
    break;
  case MONITOR:
    sem_wait(mutex_s);
    T->state[i] = HUNGRY;
    while (T->state[LEFT(i)] == EATING || T->state[RIGHT(i)] == EATING)
      cond_wait(self_c[i], mutex_s);
    T->state[i] = EATING;
    sem_post(mutex_s);
    break;
  }
}

static void
putdown(int i)
{
  switch (mode) {
  case NAIVE:
  case ORDER:
    sem_post(chop[i]);
    sem_post(chop[RIGHT(i)]);
    break;
  case ROOM:
    sem_post(chop[i]);
    sem_post(chop[RIGHT(i)]);
    sem_post(room_s);
    break;
  case TANENBAUM:
    sem_wait(mutex_s);
    T->state[i] = THINKING;
    test(LEFT(i));
    test(RIGHT(i));
    sem_post(mutex_s);
    break;
  case MONITOR:
    sem_wait(mutex_s);
    T->state[i] = THINKING;
    cond_signal(self_c[LEFT(i)]);
    cond_signal(self_c[RIGHT(i)]);
    sem_post(mutex_s);
    break;
  }
}

static void
philosopher(int i)
{
  uint seed = 31 * i + 7;

  for (int m = 0; m < MEALS; m++) {
    say(i, "is thinking");
    spin(rnd(&seed) % 700);
    pickup(i);
    eat(i, &seed);
    putdown(i);
  }
  __atomic_fetch_add(&T->done, 1, __ATOMIC_SEQ_CST);
}

static const char *names[] = { "naive", "order", "room", "tanenbaum", "monitor" };

static int
run(int m)
{
  int i, bad = 0, waited = 0;

  mode = m;
  memset(T, 0, sizeof(*T));
  for (i = 0; i < N; i++) {
    chop[i] = sem_init(1);
    self_s[i] = sem_init(0);
    self_c[i] = cond_init();
  }
  room_s = sem_init(N - 1);
  mutex_s = sem_init(1);
  out_s = sem_init(1);
  if (out_s < 0 || self_c[N - 1] < 0) {
    printf("philo: out of kernel sync objects\n");
    return 1;
  }

  for (i = 0; i < N; i++)
    spawn(philosopher, i);

  // Poll instead of just waiting, so that a deadlock gets noticed.
  while (T->done < N && waited < 300) {
    nap(1);
    waited++;
  }

  if (T->done < N) {
    for (i = 0; i < nkids; i++)
      kill(kids[i]);
    reap();
    if (m == NAIVE) {
      printf("philo naive: deadlock after %d of %d meals, as expected: PASS\n",
             T->meals[0] + T->meals[1] + T->meals[2] + T->meals[3] + T->meals[4], N * MEALS);
      bad = 0;
    } else {
      printf("philo %s: stuck, %d of %d philosophers finished: FAILED\n", names[m], T->done, N);
      bad = 1;
    }
  } else {
    bad = reap();
    int lo = MEALS, hi = 0;
    for (i = 0; i < N; i++) {
      if (T->meals[i] < lo) lo = T->meals[i];
      if (T->meals[i] > hi) hi = T->meals[i];
    }
    if (T->violations || lo != MEALS || hi != MEALS)
      bad++;
    if (m == NAIVE) {
      printf("philo naive: no deadlock this time, which should not happen: FAILED\n");
      bad++;
    } else {
      printf("philo %s: everybody ate %d times, neighbours clashed %d times: %s\n",
             names[m], hi, T->violations, bad ? "FAILED" : "ok");
    }
  }

  for (i = 0; i < N; i++) {
    sem_destroy(chop[i]);
    sem_destroy(self_s[i]);
    cond_destroy(self_c[i]);
  }
  sem_destroy(room_s);
  sem_destroy(mutex_s);
  sem_destroy(out_s);
  return bad != 0;
}

int
main(int argc, char *argv[])
{
  int first = ORDER, last = MONITOR, failed = 0;

  for (int a = 1; a < argc; a++) {
    if (strcmp(argv[a], "-v") == 0) {
      verbose = 1;
      continue;
    }
    first = -1;
    for (int m = 0; m <= MONITOR; m++)
      if (strcmp(argv[a], names[m]) == 0)
        first = last = m;
    if (first < 0) {
      fprintf(2, "usage: philo [naive|order|room|tanenbaum|monitor] [-v]\n");
      exit(2);
    }
  }

  T = (struct table *)shm_map(1);
  if ((long)T == -1) {
    printf("philo: shm_map failed\n");
    exit(1);
  }
  for (int m = first; m <= last; m++)
    failed += run(m);

  printf("philo: %s\n", failed ? "FAILED" : "PASS");
  exit(failed != 0);
}

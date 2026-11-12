// Readers-writers with semaphores, both classic variants (Courtois,
// Heymans, Parnas 1971):
//
//   reader preference   readers never wait for a writer that is merely
//                       waiting; writers can starve
//   writer preference   as soon as a writer is waiting no new reader may
//                       start; readers can starve
//
//   rwlock                    everything below
//   rwlock reader|writer      workload + scenario for just that variant
//
// Workload: several readers and writers hammer one shared record. Writers
// update it in two steps with a gap in between, so any reader that gets in
// while a writer is active sees the halves disagree. Entry checks also
// count who is inside.
//
// Scenario: the difference between the two variants, made visible.
//   t=0   reader A takes the lock and holds it for a while
//   t=2   writer W arrives and has to wait for A
//   t=4   reader B arrives
// With reader preference B slips in next to A, ahead of W. With writer
// preference B has to wait behind W.

#include "kernel/types.h"
#include "user/user.h"
#include "user/ptest.h"

struct rw {
  int readcount, writecount;   // guarded by rmutex / wmutex
  int active_r, active_w;      // who is really inside, for the checks
  int maxr;                    // most readers inside together
  int a, b;                    // the record; a == b whenever no writer is mid-update
  int nlog;
  int log[8];                  // scenario events, in order
};

enum { A_IN = 1, A_OUT, W_IN, W_OUT, B_IN, B_OUT };

static struct rw *R;
static int pref;                 // 0 reader preference, 1 writer preference
static int rmutex, wmutex, wrt, readtry, resource;   // semaphore handles

// ---- the two protocols ------------------------------------------------

static void
read_lock(void)
{
  if (pref == 0) {
    sem_wait(rmutex);
    if (++R->readcount == 1)
      sem_wait(wrt);
    sem_post(rmutex);
  } else {
    sem_wait(readtry);
    sem_wait(rmutex);
    if (++R->readcount == 1)
      sem_wait(resource);
    sem_post(rmutex);
    sem_post(readtry);
  }
}

static void
read_unlock(void)
{
  sem_wait(rmutex);
  if (--R->readcount == 0)
    sem_post(pref == 0 ? wrt : resource);
  sem_post(rmutex);
}

static void
write_lock(void)
{
  if (pref == 0) {
    sem_wait(wrt);
  } else {
    sem_wait(wmutex);
    if (++R->writecount == 1)
      sem_wait(readtry);          // first waiting writer shuts the door on readers
    sem_post(wmutex);
    sem_wait(resource);
  }
}

static void
write_unlock(void)
{
  if (pref == 0) {
    sem_post(wrt);
  } else {
    sem_post(resource);
    sem_wait(wmutex);
    if (--R->writecount == 0)
      sem_post(readtry);          // last writer reopens it
    sem_post(wmutex);
  }
}

// ---- workload ---------------------------------------------------------

#define READERS 4
#define WRITERS 2
#define READS   40
#define WRITES  20

static void
reader(int id)
{
  uint seed = 77 + id;

  for (int i = 0; i < READS; i++) {
    spin(rnd(&seed) % 400);
    read_lock();
    int in = __atomic_add_fetch(&R->active_r, 1, __ATOMIC_SEQ_CST);
    CHECK(__atomic_load_n(&R->active_w, __ATOMIC_SEQ_CST) == 0, "reader inside with a writer");
    int m = R->maxr;
    while (in > m && !__atomic_compare_exchange_n(&R->maxr, &m, in, 0,
           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      ;
    int x = R->a;
    spin(rnd(&seed) % 300);
    if (i % 10 == 0)
      nap(1);                   // a long read, so that readers pile up
    CHECK(x == R->b, "reader saw a half-written record");
    __atomic_sub_fetch(&R->active_r, 1, __ATOMIC_SEQ_CST);
    read_unlock();
  }
}

static void
writer(int id)
{
  uint seed = 99 + id;

  for (int i = 0; i < WRITES; i++) {
    spin(rnd(&seed) % 800);
    write_lock();
    int w = __atomic_fetch_add(&R->active_w, 1, __ATOMIC_SEQ_CST);
    CHECK(w == 0, "two writers inside");
    CHECK(__atomic_load_n(&R->active_r, __ATOMIC_SEQ_CST) == 0, "writer inside with readers");
    int v = R->a;
    R->a = v + 1;
    spin(rnd(&seed) % 300);     // readers would see a != b here
    R->b = v + 1;
    __atomic_sub_fetch(&R->active_w, 1, __ATOMIC_SEQ_CST);
    write_unlock();
  }
}

static const char *pname[] = { "reader preference", "writer preference" };

static int
workload(void)
{
  int i;

  for (i = 0; i < READERS; i++)
    spawn(reader, i);
  for (i = 0; i < WRITERS; i++)
    spawn(writer, i);
  watchdog(600);
  int bad = reap();
  if (R->a != WRITERS * WRITES || R->b != R->a) {
    printf("rwlock: lost updates, record is %d/%d, expected %d\n", R->a, R->b, WRITERS * WRITES);
    bad++;
  }
  if (R->maxr < 2) {
    printf("rwlock: readers never overlapped, that is not a readers-writers lock\n");
    bad++;
  }
  printf("rwlock %s: %d readers + %d writers, record = %d, up to %d readers at once: %s\n",
         pname[pref], READERS, WRITERS, R->a, R->maxr, bad ? "FAILED" : "ok");
  return bad != 0;
}

// ---- scenario ---------------------------------------------------------

static void
logev(int ev)
{
  int i = __atomic_fetch_add(&R->nlog, 1, __ATOMIC_SEQ_CST);
  R->log[i] = ev;
}

static int
position(int ev)
{
  for (int i = 0; i < R->nlog; i++)
    if (R->log[i] == ev)
      return i;
  return -1;
}

static void
scen_reader_a(int arg)
{
  read_lock();
  logev(A_IN);
  nap(10);
  logev(A_OUT);
  read_unlock();
}

static void
scen_writer(int arg)
{
  write_lock();
  logev(W_IN);
  nap(2);
  logev(W_OUT);
  write_unlock();
}

static void
scen_reader_b(int arg)
{
  read_lock();
  logev(B_IN);
  nap(2);
  logev(B_OUT);
  read_unlock();
}

static int
scenario(void)
{
  static const char *evname[] = { "?", "A in", "A out", "W in", "W out", "B in", "B out" };
  int bad = 0;

  R->nlog = 0;
  spawn(scen_reader_a, 0);
  nap(2);
  spawn(scen_writer, 0);
  nap(2);
  spawn(scen_reader_b, 0);
  watchdog(300);
  bad = reap();

  printf("rwlock %s, scenario: ", pname[pref]);
  for (int i = 0; i < R->nlog; i++)
    printf("%s%s", i ? ", " : "", evname[R->log[i]]);
  printf("\n");

  int w = position(W_IN), b = position(B_IN);
  if (R->nlog != 6 || w < 0 || b < 0) {
    printf("rwlock: scenario did not run to completion\n");
    return 1;
  }
  if (pref == 0 && !(b < w)) {
    printf("rwlock: reader preference should let B in before W\n");
    bad++;
  }
  if (pref == 1 && !(w < b)) {
    printf("rwlock: writer preference should make B wait for W\n");
    bad++;
  }
  // and in no case may W overlap with anybody
  if (position(W_IN) < position(A_OUT) || (position(B_IN) > position(W_IN) && position(B_IN) < position(W_OUT))) {
    printf("rwlock: writer overlapped with a reader\n");
    bad++;
  }
  return bad != 0;
}

// ---- main -------------------------------------------------------------

static int
run(int p)
{
  int bad;

  pref = p;
  memset(R, 0, sizeof(*R));
  rmutex = sem_init(1);
  wmutex = sem_init(1);
  wrt = sem_init(1);
  readtry = sem_init(1);
  resource = sem_init(1);
  if (rmutex < 0 || wmutex < 0 || wrt < 0 || readtry < 0 || resource < 0) {
    printf("rwlock: out of semaphores\n");
    return 1;
  }
  bad = workload();
  memset(R, 0, sizeof(*R));
  bad += scenario();

  sem_destroy(rmutex);
  sem_destroy(wmutex);
  sem_destroy(wrt);
  sem_destroy(readtry);
  sem_destroy(resource);
  return bad != 0;
}

int
main(int argc, char *argv[])
{
  int first = 0, last = 1, failed = 0;

  if (argc > 1) {
    if (strcmp(argv[1], "reader") == 0)
      first = last = 0;
    else if (strcmp(argv[1], "writer") == 0)
      first = last = 1;
    else {
      fprintf(2, "usage: rwlock [reader|writer]\n");
      exit(2);
    }
  }
  R = (struct rw *)shm_map(1);
  if ((long)R == -1) {
    printf("rwlock: shm_map failed\n");
    exit(1);
  }
  for (int p = first; p <= last; p++)
    failed += run(p);
  printf("rwlock: %s\n", failed ? "FAILED" : "PASS");
  exit(failed != 0);
}

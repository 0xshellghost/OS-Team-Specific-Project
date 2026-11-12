// Producer-consumer with a bounded buffer, solved three ways:
//
//   sem     three semaphores: empty slots, full slots, and a mutex
//   cond    kernel condition variables (not-full, not-empty) + a semaphore mutex
//   futex   the same shape on the user-level futex mutex and condvar
//
//   pcbuf                         all three with the defaults
//   pcbuf sem|cond|futex [producers [consumers [items-per-producer]]]
//
// The buffer is deliberately tiny (4 slots) so that producers really have
// to wait for consumers and the other way round. Every item has a unique
// id; at the end each id must have been consumed exactly once, and each
// consumer must have seen the items of one producer in increasing order.

#include "kernel/types.h"
#include "user/user.h"
#include "user/usync.h"
#include "user/ptest.h"

#define BUFSZ    4
#define MAXITEMS 1500
#define MAXPROD  8

struct shared {
  int buf[BUFSZ];
  int in, out, count;
  int maxcount;           // fullest the buffer ever got
  int seen[MAXITEMS];     // times each item id has been consumed
  fmutex_t fm;            // futex mode only
  fcond_t fnotfull, fnotempty;
};

static struct shared *S;
static int mode;                 // 0 sem, 1 cond, 2 futex
static int nprod, ncons, per_prod, per_cons;
static int empty_s, full_s, mutex_s, notfull_c, notempty_c;

// These two run with the buffer lock held, whatever kind of lock it is.
static void
buf_put(int item)
{
  S->buf[S->in] = item;
  S->in = (S->in + 1) % BUFSZ;
  S->count++;
  CHECK(S->count <= BUFSZ, "buffer overflow");
  if (S->count > S->maxcount)
    S->maxcount = S->count;
}

static int
buf_get(void)
{
  int item;

  CHECK(S->count > 0, "buffer underflow");
  item = S->buf[S->out];
  S->out = (S->out + 1) % BUFSZ;
  S->count--;
  return item;
}

static void
produce(int item)
{
  switch (mode) {
  case 0:
    sem_wait(empty_s);
    sem_wait(mutex_s);
    buf_put(item);
    sem_post(mutex_s);
    sem_post(full_s);
    break;
  case 1:
    sem_wait(mutex_s);
    while (S->count == BUFSZ)
      cond_wait(notfull_c, mutex_s);
    buf_put(item);
    cond_signal(notempty_c);
    sem_post(mutex_s);
    break;
  case 2:
    fmutex_lock(&S->fm);
    while (S->count == BUFSZ)
      fcond_wait(&S->fnotfull, &S->fm);
    buf_put(item);
    fcond_signal(&S->fnotempty);
    fmutex_unlock(&S->fm);
    break;
  }
}

static int
consume(void)
{
  int item = -1;

  switch (mode) {
  case 0:
    sem_wait(full_s);
    sem_wait(mutex_s);
    item = buf_get();
    sem_post(mutex_s);
    sem_post(empty_s);
    break;
  case 1:
    sem_wait(mutex_s);
    while (S->count == 0)
      cond_wait(notempty_c, mutex_s);
    item = buf_get();
    cond_signal(notfull_c);
    sem_post(mutex_s);
    break;
  case 2:
    fmutex_lock(&S->fm);
    while (S->count == 0)
      fcond_wait(&S->fnotempty, &S->fm);
    item = buf_get();
    fcond_signal(&S->fnotfull);
    fmutex_unlock(&S->fm);
    break;
  }
  return item;
}

static void
producer(int id)
{
  uint seed = 1000 + id;

  for (int k = 0; k < per_prod; k++) {
    spin(rnd(&seed) % 300);
    produce(id * per_prod + k);
  }
}

static void
consumer(int id)
{
  uint seed = 5000 + id;
  int last[MAXPROD];

  for (int i = 0; i < MAXPROD; i++)
    last[i] = -1;
  for (int k = 0; k < per_cons; k++) {
    spin(rnd(&seed) % 300);
    int item = consume();
    CHECK(item >= 0 && item < nprod * per_prod, "consumed garbage");
    int p = item / per_prod;
    CHECK(item > last[p], "one producer's items arrived out of order");
    last[p] = item;
    __atomic_fetch_add(&S->seen[item], 1, __ATOMIC_SEQ_CST);
  }
}

static const char *names[] = { "sem", "cond", "futex" };

static int
run(int m)
{
  int i, total = nprod * per_prod;

  mode = m;
  memset(S, 0, sizeof(*S));
  fmutex_init(&S->fm);
  fcond_init(&S->fnotfull);
  fcond_init(&S->fnotempty);
  empty_s = sem_init(BUFSZ);
  full_s = sem_init(0);
  mutex_s = sem_init(1);
  notfull_c = cond_init();
  notempty_c = cond_init();
  if (empty_s < 0 || full_s < 0 || mutex_s < 0 || notfull_c < 0 || notempty_c < 0) {
    printf("pcbuf: out of kernel sync objects\n");
    return 1;
  }

  for (i = 0; i < nprod; i++)
    spawn(producer, i);
  for (i = 0; i < ncons; i++)
    spawn(consumer, i);
  watchdog(600);
  int bad = reap();

  for (i = 0; i < total; i++)
    if (S->seen[i] != 1) {
      printf("pcbuf %s: item %d consumed %d times\n", names[m], i, S->seen[i]);
      bad++;
      break;
    }
  if (S->count != 0) {
    printf("pcbuf %s: %d items left in the buffer\n", names[m], S->count);
    bad++;
  }

  printf("pcbuf %s: %d producers, %d consumers, %d items, buffer peaked at %d/%d: %s\n",
         names[m], nprod, ncons, total, S->maxcount, BUFSZ, bad ? "FAILED" : "ok");

  sem_destroy(empty_s);
  sem_destroy(full_s);
  sem_destroy(mutex_s);
  cond_destroy(notfull_c);
  cond_destroy(notempty_c);
  return bad != 0;
}

int
main(int argc, char *argv[])
{
  int first = 0, last = 2, failed = 0;

  nprod = 3;
  ncons = 2;
  per_prod = 120;  // 360 items; divisible by both 3 and 2 consumers
  if (argc > 1) {
    first = last = -1;
    for (int i = 0; i < 3; i++)
      if (strcmp(argv[1], names[i]) == 0)
        first = last = i;
    if (first < 0) {
      fprintf(2, "usage: pcbuf [sem|cond|futex [producers [consumers [items]]]]\n");
      exit(2);
    }
  }
  if (argc > 2) nprod = atoi(argv[2]);
  if (argc > 3) ncons = atoi(argv[3]);
  if (argc > 4) per_prod = atoi(argv[4]);

  if (nprod < 1 || nprod > MAXPROD || ncons < 1 || ncons > 16 ||
      per_prod < 1 || nprod * per_prod > MAXITEMS ||
      (nprod * per_prod) % ncons != 0) {
    fprintf(2, "pcbuf: need 1..%d producers, 1..16 consumers, at most %d items in "
            "total, and the total must divide evenly among the consumers\n",
            MAXPROD, MAXITEMS);
    exit(2);
  }
  per_cons = nprod * per_prod / ncons;

  S = (struct shared *)shm_map((sizeof(struct shared) + 4095) / 4096);
  if ((long)S == -1) {
    printf("pcbuf: shm_map failed\n");
    exit(1);
  }

  for (int m = first; m <= last; m++)
    failed += run(m);

  printf("pcbuf: %s\n", failed ? "FAILED" : "PASS");
  exit(failed != 0);
}

// Small helpers shared by the project 5 test and demo programs.
// Everything is static inline so each program just includes this.
//
// The shape of every test is: parent builds shared state with shm_map(),
// spawn()s a few workers, maybe sleeps a while to let them block, pokes
// them, then reap()s them. reap() returns how many workers failed. A
// watchdog process turns a deadlock into a FAIL instead of a hung qemu.

#include "kernel/types.h"
#include "user/user.h"

#define MAXKID 32

static int kids[MAXKID];
static int nkids;
static int dogpid;

// Fail the whole program (nonzero exit) with a message.
#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
      printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);          \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

// ticks of the timer, roughly 100 ms each under qemu
static inline void nap(int ticks) { pause(ticks); }

// short busy delay, to make races more likely without a system call
static inline void spin(int n)
{
  for (volatile int i = 0; i < n; i++)
    ;
}

// tiny LCG so every process can have its own pseudo random stream
static inline uint rnd(uint *seed)
{
  *seed = *seed * 1103515245 + 12345;
  return (*seed >> 16) & 0x7fff;
}

// Start fn(arg) in a child process. The child exits 0 when fn returns.
static inline int spawn(void (*fn)(int), int arg)
{
  int pid;

  if (nkids >= MAXKID) {
    printf("FAIL: too many workers\n");
    exit(1);
  }
  pid = fork();
  if (pid < 0) {
    printf("FAIL: fork\n");
    exit(1);
  }
  if (pid == 0) {
    fn(arg);
    exit(0);
  }
  kids[nkids++] = pid;
  return pid;
}

// After this many ticks without reap() being done, kill everything,
// including the caller, and say so. Call after the spawn()s: the watchdog
// is a fork and has to know the pids.
static inline void watchdog(int ticks)
{
  int parent = getpid();
  int pid = fork();

  if (pid == 0) {
    nap(ticks);
    printf("FAIL: watchdog fired after %d ticks (deadlock?)\n", ticks);
    for (int i = 0; i < nkids; i++)
      kill(kids[i]);
    kill(parent);
    exit(0);
  }
  dogpid = pid;
}

// Wait for every spawn()ed worker. Returns how many exited nonzero.
static inline int reap(void)
{
  int left = nkids, bad = 0, st, pid;

  while (left > 0) {
    pid = wait(&st);
    if (pid < 0)
      break;
    if (pid == dogpid) { // watchdog went off first
      dogpid = 0;
      continue;
    }
    if (st != 0)
      bad++;
    left--;
  }
  if (dogpid > 0) {
    kill(dogpid);
    wait(0);
    dogpid = 0;
  }
  nkids = 0;
  return bad;
}

// Stop one worker and collect it. Returns its exit status.
static inline int kill_and_wait(int pid)
{
  int st = 0;

  kill(pid);
  for (;;) {
    int r = wait(&st);
    if (r == pid || r < 0)
      break;
  }
  for (int i = 0; i < nkids; i++)
    if (kids[i] == pid)
      kids[i] = kids[--nkids];
  return st;
}

static inline int *shm_ints(int npages)
{
  void *p = shm_map(npages);
  if (p == (void *)-1) {
    printf("FAIL: shm_map\n");
    exit(1);
  }
  return (int *)p;
}

// Kernel lock-contention profiler CLI and analysis tool.
//
// Usage:
//   hotlocks                  run multi-core benchmark and find hot locks
//   hotlocks dump             display current lock stats without resetting
//   hotlocks raw              display per-spinlock unaggregated stats
//   hotlocks reset            reset all lock contention counters
//   hotlocks <prog> [args...] profile a specific command (e.g. hotlocks p5test)

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define MAX_ENTRIES 64

static void
print_stat_row(struct lock_stat *s)
{
  uint64 cont_pct_int = 0;
  uint64 cont_pct_frac = 0;
  uint64 avg_spin_int = 0;
  uint64 avg_spin_frac = 0;

  if (s->n_acq > 0) {
    uint64 pct10 = (s->n_contended * 1000) / s->n_acq;
    cont_pct_int = pct10 / 10;
    cont_pct_frac = pct10 % 10;
  }
  if (s->n_contended > 0) {
    uint64 avg10 = (s->n_spins * 10) / s->n_contended;
    avg_spin_int = avg10 / 10;
    avg_spin_frac = avg10 % 10;
  }

  // Format columns: Name (12) | Acquires (10) | Contended (10) | Cont% (7) | Total Spins (12) | Avg Spins/Cont (14)
  printf("%s", s->name);
  int len = strlen(s->name);
  for (int i = len; i < 14; i++)
    printf(" ");

  printf("%d", (int)s->n_acq);
  // Pad acquisitions
  int acq_digits = 1;
  uint64 t = s->n_acq;
  while (t >= 10) { acq_digits++; t /= 10; }
  for (int i = acq_digits; i < 12; i++) printf(" ");

  printf("%d", (int)s->n_contended);
  int cont_digits = 1;
  t = s->n_contended;
  while (t >= 10) { cont_digits++; t /= 10; }
  for (int i = cont_digits; i < 12; i++) printf(" ");

  printf("%d.%d%%", (int)cont_pct_int, (int)cont_pct_frac);
  int pct_len = 4 + (cont_pct_int >= 10 ? 1 : 0) + (cont_pct_int >= 100 ? 1 : 0);
  for (int i = pct_len; i < 10; i++) printf(" ");

  printf("%d", (int)s->n_spins);
  int spin_digits = 1;
  t = s->n_spins;
  while (t >= 10) { spin_digits++; t /= 10; }
  for (int i = spin_digits; i < 14; i++) printf(" ");

  printf("%d.%d\n", (int)avg_spin_int, (int)avg_spin_frac);
}

static void
sort_stats(struct lock_stat *stats, int n)
{
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (stats[j].n_spins < stats[j + 1].n_spins ||
          (stats[j].n_spins == stats[j + 1].n_spins && stats[j].n_contended < stats[j + 1].n_contended)) {
        struct lock_stat tmp = stats[j];
        stats[j] = stats[j + 1];
        stats[j + 1] = tmp;
      }
    }
  }
}

static void
display_stats(int mode)
{
  struct lock_stat stats[MAX_ENTRIES];
  int n = lockstat(stats, MAX_ENTRIES, mode);
  if (n < 0) {
    printf("hotlocks: lockstat failed\n");
    return;
  }

  sort_stats(stats, n);

  printf("\n======================== XV6 LOCK CONTENTION PROFILER ========================\n");
  printf("LOCK NAME     ACQUIRES    CONTENDED   CONT RATE   TOTAL SPINS   AVG SPINS/CONT\n");
  printf("------------------------------------------------------------------------------\n");

  int hot_count = 0;
  for (int i = 0; i < n; i++) {
    if (stats[i].n_acq > 0 || stats[i].n_spins > 0) {
      print_stat_row(&stats[i]);
      if (stats[i].n_spins > 0)
        hot_count++;
    }
  }
  printf("------------------------------------------------------------------------------\n");

  printf("\n=== Analysis of Hot Locks in xv6 ===\n");
  for (int i = 0; i < n && i < 4; i++) {
    if (stats[i].n_spins == 0)
      continue;
    if (strcmp(stats[i].name, "kmem") == 0) {
      printf("[*] 'kmem' (Memory Allocator): Heavy contention due to single global freelist lock.\n"
             "    Concurrent page allocations (fork, exec, sbrk) on multiple CPUs fight for this lock.\n");
    } else if (strcmp(stats[i].name, "bcache") == 0) {
      printf("[*] 'bcache' (Buffer Cache): Contention from concurrent block reads/writes.\n"
             "    A single lock protects all buffer headers and the LRU list.\n");
    } else if (strcmp(stats[i].name, "wait_lock") == 0 || strcmp(stats[i].name, "proc") == 0) {
      printf("[*] '%s' (Process Management): Contention during concurrent fork/exit/wait\n"
             "    and process table state transitions.\n", stats[i].name);
    } else if (strcmp(stats[i].name, "futex") == 0 || strcmp(stats[i].name, "sem") == 0 || strcmp(stats[i].name, "semtab") == 0) {
      printf("[*] '%s' (Sync Primitives): Contention from concurrent waiter queues.\n", stats[i].name);
    }
  }
  if (hot_count == 0) {
    printf("(No lock spin contention observed in this run - single CPU or uncontended locks)\n");
  }
  printf("==============================================================================\n\n");
}

static void
run_stress_workload(void)
{
  int nworkers = 4;
  printf("hotlocks: running multi-core workload across %d worker processes...\n", nworkers);

  for (int w = 0; w < nworkers; w++) {
    int pid = fork();
    if (pid < 0) {
      printf("hotlocks: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      // Workload 1: Memory allocator stress (kmem lock)
      for (int i = 0; i < 300; i++) {
        char *p = sbrk(4096);
        if (p != (char *)-1) {
          p[0] = (char)i;
          sbrk(-4096);
        }
      }

      // Workload 2: Filesystem buffer cache stress (bcache / itable)
      char fname[16];
      fname[0] = 'l'; fname[1] = 'k'; fname[2] = '0' + w; fname[3] = '\0';
      for (int i = 0; i < 20; i++) {
        int fd = open(fname, O_CREATE | O_RDWR);
        if (fd >= 0) {
          write(fd, "test", 4);
          close(fd);
          unlink(fname);
        }
      }

      // Workload 3: Short fork/exit (proc and wait_lock)
      for (int i = 0; i < 8; i++) {
        int k = fork();
        if (k == 0) exit(0);
        wait(0);
      }

      exit(0);
    }
  }

  for (int w = 0; w < nworkers; w++) {
    wait(0);
  }
  printf("hotlocks: workload completed.\n");
}

int
main(int argc, char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "reset") == 0) {
    if (lockstat(0, 0, LOCKSTAT_RESET) == 0)
      printf("hotlocks: lock contention counters reset to zero.\n");
    else
      printf("hotlocks: reset failed.\n");
    exit(0);
  }

  if (argc == 2 && strcmp(argv[1], "dump") == 0) {
    display_stats(LOCKSTAT_AGGREGATE);
    exit(0);
  }

  if (argc == 2 && strcmp(argv[1], "raw") == 0) {
    display_stats(LOCKSTAT_RAW);
    exit(0);
  }

  if (argc >= 2) {
    // Run custom command with profiling
    printf("hotlocks: resetting counters and profiling command '%s'...\n", argv[1]);
    lockstat(0, 0, LOCKSTAT_RESET);

    int pid = fork();
    if (pid < 0) {
      printf("hotlocks: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      exec(argv[1], &argv[1]);
      printf("hotlocks: cannot exec %s\n", argv[1]);
      exit(1);
    }
    int status = 0;
    wait(&status);
    printf("\nhotlocks: target program exited with status %d\n", status);
    display_stats(LOCKSTAT_AGGREGATE);
    exit(status);
  }

  // Default: reset, run workload, show hot locks
  lockstat(0, 0, LOCKSTAT_RESET);
  run_stress_workload();
  display_stats(LOCKSTAT_AGGREGATE);
  printf("hotlocks: PASS\n");
  exit(0);
}

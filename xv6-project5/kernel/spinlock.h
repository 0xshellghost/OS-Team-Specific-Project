// Mutual exclusion lock.
struct spinlock {
  uint locked; // Is the lock held?

  // For debugging:
  char *name;      // Name of lock.
  struct cpu *cpu; // The cpu holding the lock.

  // Lock contention profiling:
  uint64 n_spins;     // Total spin iterations spent waiting in acquire()
  uint64 n_acq;       // Total number of acquire() calls
  uint64 n_contended; // Number of acquires that encountered contention
};

#ifndef _LOCK_STAT_DEFINED
#define _LOCK_STAT_DEFINED
#define LOCK_NAME_MAX 16
#define LOCKSTAT_AGGREGATE 0
#define LOCKSTAT_RESET     1
#define LOCKSTAT_RAW       2

struct lock_stat {
  char name[LOCK_NAME_MAX];
  uint64 n_acq;
  uint64 n_contended;
  uint64 n_spins;
};
#endif


# Project 5 – Semaphores, Condition Variables, Futexes, and Lock Profiling

Target: xv6-riscv, MIT master as of 2026-11 (commit `fa2ddcb`, the one with
`sleep_prepare()`/`sleep()`). Nothing in the new code calls `sleep()`, so
the kernel half should drop onto older xv6-riscv trees with only small edits
(see "What is not done").

## What was added

### Kernel

| file | what it does |
|---|---|
| `kernel/waitq.[ch]` | FIFO wait queue. Nodes live on the kernel stack. `wq_block()` is a hand-written sleep that also checks `p->killed` under `p->lock`, so a `kill()` cannot slip in between releasing the queue lock and going to sleep. |
| `kernel/sem.c` | Counting semaphores and condition variables, plus their system calls. |
| `kernel/futex.c` | `futex_wait` / `futex_wake`, hashed buckets keyed on the *physical* address of the word. |
| `kernel/shm.c`, `vm.c` | `shm_map()`: pages that `fork()` shares instead of copying (software PTE bit `PTE_SHM`). |
| `kernel/kalloc.c` | Per-page reference counts, so a shared page is freed exactly once, by its last user. |
| `proc.c`, `exec.c`, `sysproc.c` | fork/exit/exec bookkeeping for the shared window, handles, and syscall dispatch. |
| `kernel/spinlock.[ch]` | Lock-contention profiler: tracks spin iterations, total acquires, and contention events per spinlock; registry and `lockstat` kernel copyout. |

### System calls

```c
int   sem_init(int value);          // -> handle, or -1
int   sem_wait(int sd);             // 0, or -1
int   sem_trywait(int sd);          // 0 got it, 1 would block, -1 error
int   sem_post(int sd);
int   sem_getvalue(int sd);
int   sem_destroy(int sd);          // drops this process's handle

int   cond_init(void);
int   cond_wait(int cd, int sd);    // sd = semaphore used as the mutex
int   cond_signal(int cd);
int   cond_broadcast(int cd);
int   cond_destroy(int cd);

int   futex_wait(int *addr, int val);   // 0 woken, 1 *addr != val, -1 error/killed
int   futex_wake(int *addr, int n);     // number woken (n < 0: all)

void *shm_map(int npages);          // zeroed pages shared with future children

int   lockstat(struct lock_stat *stats, int max, int mode); // 0=aggregate, 1=reset, 2=raw
```

### User space

* `user/usync.[ch]` – futex mutex (3-state, Drepper) and futex condition variable.
* `user/semtest.c`, `condtest.c`, `futextest.c`, `shmtest.c`, `locktest.c` – unit tests.
* `user/pcbuf.c` – bounded buffer: semaphores / kernel condvars / futex.
* `user/rwlock.c` – readers-writers, reader preference and writer preference,
  with a workload and a timed scenario that shows the difference.
* `user/philo.c` – dining philosophers: naive (deadlocks on purpose),
  resource ordering, N-1 seats, Tanenbaum state array, monitor with condvars.
* `user/hotlocks.c` – lock-contention profiler CLI and hot lock analyzer tool.
* `user/p5test.c` – runs everything (now all 9 test suites). `test-p5.py` boots qemu and drives it.

## Design decisions worth knowing

**Why `shm_map()` exists.** xv6 has no threads and processes share nothing, so
a semaphore has nothing to protect. `shm_map()` is the smallest fix. If your
xv6 already has `clone()`-style threads you can ignore it: the primitives are
keyed on physical addresses and work for threads unchanged.

**Handles behave like file descriptors.** Per-process bitmask, inherited by
`fork()`, reference counted, released at `exit()`. A crashed process cannot leak
table slots, and stale or invented handles are rejected.

**Semaphores are strict FIFO with direct hand-off.** `sem_post()` with sleepers
gives the token to the oldest waiter instead of incrementing the counter, so
nobody can barge in and nobody starves. Invariant: `value > 0` implies empty queue.

**Condition variables are Mesa style.** `cond_wait` enqueues itself *before* it
releases the mutex (under the condvar's lock), so a signal cannot fall in the
gap. Lock order is cond lock, then semaphore lock, then `p->lock`. `cond_wait`
refuses a mutex whose value is not 0 (plainly unlocked).

**Futex.** The compare and the sleep happen under the bucket lock; wake takes the
same lock. The load of the user word is atomic. Wait returns 1 instead of
sleeping on a mismatch. Wake never faults a lazily-allocated page in.

**Kill safety.** A killed sleeper removes its own node from the queue. If a wake
and a kill race, the wake wins and the token is not lost.

**Lock-Contention Profiler.**
* In `acquire()`: an iteration counter `spins` measures iterations spent spinning
  in `while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0)`.
* When the lock is successfully claimed, the CPU updates `lk->n_acq++`,
  `lk->n_contended++`, and `lk->n_spins += spins` safely without additional locks
  because the spinlock is now exclusively owned by the calling CPU.
* Every lock registers in `lock_profiler_register()` during `initlock()`.
* `lockstat` syscall supports:
  - `LOCKSTAT_AGGREGATE (0)`: rolls up contention metrics across all locks sharing
    the same name (e.g. all 64 process locks under `"proc"`, all 16 futex buckets).
  - `LOCKSTAT_RESET (1)`: atomically clears spin and acquisition counters.
  - `LOCKSTAT_RAW (2)`: extracts unaggregated per-instance lock structures.

## Hot Locks in xv6 (Empirical Findings)

Running `hotlocks` on multi-core QEMU (`CPUS=3`) identifies the primary lock bottlenecks in xv6:

1. **`kmem` (Physical Page Allocator)**:
   * **Why it contends**: xv6 uses a single global free list protected by `kmem.lock`.
     When multiple CPUs concurrently allocate and deallocate pages (e.g. during `fork`,
     `exec`, `sbrk`, or IPC buffer creation), all CPUs bottleneck on `kmem.lock`.
   * **Mitigation (e.g. MIT lock lab)**: Per-CPU freelists with lock stealing.

2. **`bcache` (Disk Buffer Cache)**:
   * **Why it contends**: A single global spinlock `bcache.lock` guards both the buffer
     hash chains and the global LRU doubly-linked list. Multiple processes performing
     concurrent file and inode reads/writes contend severely on `bcache.lock`.
   * **Mitigation**: Hash-bucket locks instead of a single global lock.

3. **`proc` and `wait_lock` (Process Table)**:
   * **Why it contends**: When many processes fork, exit, and wait concurrently, parent
     processes scanning children hold `wait_lock`, while schedulers and state transitions
     contend on individual `p->lock` instances.

4. **Synchronization Locks (`sem`, `semtab`, `futex`)**:
   * Contend under multi-threaded producer-consumer and barrier benchmarks. The 16
     separate futex hash buckets successfully distribute contention compared to a
     monolithic lock.

## Building and testing

```
make qemu                 # then, at the shell prompt:
$ p5test                  # everything (all 9 suites)
$ locktest                # lockstat unit tests
$ hotlocks                # run multi-core benchmark and view hot locks table
$ hotlocks pcbuf          # profile lock contention during bounded buffer
$ hotlocks p5test         # profile lock contention across the entire suite
$ semtest                 # or any single program
$ pcbuf cond 4 2 100      # producers, consumers, items per producer
$ rwlock writer
$ philo monitor -v        # -v prints who eats when
```

Without typing: `./test-p5.py p5test` (needs python3; `CPUS=1` for one hart).
To run hotlocks test automatically: `./test-p5.py locktest hotlocks`.

## Limits / things to be aware of

* Semaphores and condvars are only shareable through `fork()` inheritance.
  There are no named objects.
* A process that dies holding a "mutex" semaphore leaves it locked. There is no
  owner tracking.
* `exec()` keeps semaphore/condvar handles (like fds) but drops shared memory.
* Per-process cap of 64 shared pages; 64 semaphores and 64 condvars system-wide.
* `FSSIZE` is 4000 blocks so all test and profiler programs fit.

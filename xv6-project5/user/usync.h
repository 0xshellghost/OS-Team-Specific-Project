// User-level synchronization built only on futex_wait()/futex_wake().
//
// Nothing here enters the kernel unless there is real contention, which is
// the point of futexes: an uncontended lock or a signal with nobody waiting
// is a couple of atomic instructions.
//
// All of it works on memory that is shared between processes, i.e. memory
// from shm_map(), and every object must be initialized once before fork().

typedef struct {
  int state;   // 0 unlocked, 1 locked, 2 locked and somebody may be asleep
} fmutex_t;

typedef struct {
  int seq;     // bumped by every signal; sleepers wait for it to change
  int waiters; // processes between "I'm going to wait" and "I woke up"
} fcond_t;

void fmutex_init(fmutex_t *m);
void fmutex_lock(fmutex_t *m);
int  fmutex_trylock(fmutex_t *m);   // 0 if acquired, -1 if it was taken
void fmutex_unlock(fmutex_t *m);

void fcond_init(fcond_t *c);
void fcond_wait(fcond_t *c, fmutex_t *m);   // m must be locked by the caller
void fcond_signal(fcond_t *c);
void fcond_broadcast(fcond_t *c);

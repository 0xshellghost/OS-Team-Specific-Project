// FIFO wait queues, the common plumbing under semaphores, condition
// variables and futexes (see waitq.c).
//
// A waiter node lives on the kernel stack of the process that is blocked,
// so it costs nothing to allocate and can never run out. The price is that
// it must be off the queue before the blocking function returns. wq_block()
// and the wake functions take care of that.

struct spinlock;

struct waiter {
  struct waiter *prev, *next;
  uint64 key;     // for the owner of the queue; futexes store the word's PA
  int signaled;   // set by whoever wakes us. This is the wakeup condition.
};

struct waitq {
  struct waiter head; // circular list, head is the sentinel
};

void wq_init(struct waitq *);
int  wq_empty(struct waitq *);
void wq_enqueue(struct waitq *, struct waiter *, uint64 key);
int  wq_block(struct waiter *, struct spinlock *);
int  wq_wake(struct waitq *, int n);
int  wq_wake_key(struct waitq *, uint64 key, int n);

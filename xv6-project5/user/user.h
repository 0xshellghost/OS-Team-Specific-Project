#define SBRK_ERROR ((char *)-1)

struct stat;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int *);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int close(int);
int kill(int);
int exec(const char *, char **);
int open(const char *, int);
int mknod(const char *, short, short);
int unlink(const char *);
int fstat(int fd, struct stat *);
int link(const char *, const char *);
int mkdir(const char *);
int chdir(const char *);
int dup(int);
int getpid(void);
char *sys_sbrk(int, int);
int pause(int);
int uptime(void);
int sync(void);

// project 5: semaphores, condition variables, futexes, shared memory
int sem_init(int value);
int sem_wait(int sd);
int sem_trywait(int sd);
int sem_post(int sd);
int sem_getvalue(int sd);
int sem_destroy(int sd);
int cond_init(void);
int cond_wait(int cd, int sd);
int cond_signal(int cd);
int cond_broadcast(int cd);
int cond_destroy(int cd);
int futex_wait(int *addr, int val);
int futex_wake(int *addr, int n);
void *shm_map(int npages);

// Lock contention profiler
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

int lockstat(struct lock_stat *stats, int max, int mode);

// ulib.c
int stat(const char *, struct stat *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char c);
int strcmp(const char *, const char *);
char *gets(char *, int max);
uint strlen(const char *);
void *memset(void *, int, uint);
int atoi(const char *);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char *sbrk(int);
char *sbrklazy(int);

// printf.c
void fprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void printf(const char *, ...) __attribute__((format(printf, 1, 2)));

// umalloc.c
void *malloc(uint);
void free(void *);

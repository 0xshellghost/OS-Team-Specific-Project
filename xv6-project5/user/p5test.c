// Runs the whole project 5 suite, one program after the other, and says
// at the end whether everything passed.

#include "kernel/types.h"
#include "user/user.h"

static char *suite[][4] = {
  { "semtest", 0 },
  { "condtest", 0 },
  { "futextest", 0 },
  { "shmtest", 0 },
  { "pcbuf", 0 },
  { "rwlock", 0 },
  { "philo", 0 },
  { "philo", "naive", 0 },
  { "locktest", 0 },
};

int
main(void)
{
  int n = sizeof(suite) / sizeof(suite[0]), failed = 0;

  for (int i = 0; i < n; i++) {
    int pid = fork();
    if (pid < 0) {
      printf("p5test: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      exec(suite[i][0], suite[i]);
      printf("p5test: cannot exec %s\n", suite[i][0]);
      exit(1);
    }
    int st;
    wait(&st);
    if (st != 0) {
      printf("p5test: %s exited with status %d\n", suite[i][0], st);
      failed++;
    }
  }

  if (failed)
    printf("p5test: %d of %d programs did not pass\n", failed, n);
  else
    printf("p5test: PASS (all %d programs)\n", n);
  exit(failed != 0);
}

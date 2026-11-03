#include "../src/frontend/profile.h"
#include <assert.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
  char padding[301];
  memset(padding, 'x', sizeof(padding)-1);
  padding[sizeof(padding)-1] = 0;
  for (int worker=0; worker<4; worker++) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      for (int record=0; record<1000; record++)
        txp_profile_printf("[profile] {\"worker\":%d,\"record\":%d,\"padding\":\"%s\"}\n", worker, record, padding);
      _exit(0);
    }
  }
  for (int record=0; record<1000; record++) {
    const char marker[] = "WORKER-MARKER\n";
    assert(write(STDERR_FILENO, marker, sizeof(marker)-1) == sizeof(marker)-1);
  }
  for (int worker=0; worker<4; worker++) {
    int status;
    assert(wait(&status) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  /* Oversized records must be omitted, never emitted as truncated JSON. */
  txp_profile_printf("[profile] {\"oversized\":\"%s%s\"}\n", padding, padding);
  return 0;
}

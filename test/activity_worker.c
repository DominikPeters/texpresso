/* Real worker client; the controlled frontend lives in test_activity.c. */
#include <assert.h>
#include <sys/socket.h>
#include "../src/engine/main/texpresso_protocol.h"
#include "../src/include/txp_activity.h"
int xetex_tokens;
static void busy(double ms)
{
  double start=txp_activity_clock().cpu;
  while(txp_activity_clock().cpu-start<ms) { }
}
void activity_test_worker(int fd)
{
  FILE *stream=fdopen(fd,"r+"); assert(stream);
  txp_client *io=txp_connect(stream);
  busy(25); xetex_tokens=101; assert(txp_size(io,11)==7);
  usleep(50000); xetex_tokens=202; assert(txp_mtime(io,11)==9);
  char byte; xetex_tokens=303;
  assert(txp_read(io,11,0,&byte,1)==1 && byte=='x');
  txp_seen(io,11,1); txp_flush(io);
  // CHLD follows the one-way SEEN. Child numbering restarts, parent resumes.
  pid_t child=txp_fork(io);
  if(child==0) {
    usleep(80000); xetex_tokens=505; assert(txp_size(io,11)==7);
    txp_finish(io,true); _exit(0);
  }
  busy(15); xetex_tokens=606; assert(txp_size(io,11)==7);
  txp_finish(io,true); _exit(0);
}

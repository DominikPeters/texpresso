#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "../src/frontend/sprotocol.h"
void activity_test_worker(int fd);
// Keep this standalone protocol test independent of the renderer/common library.
void print_backtrace(void) { fputs("controlled protocol test aborted\n",stderr); }
static query_t next(channel_t *c,int fd,int pid,uint64_t *ordinal)
{
  channel_profile_context(c,pid,*ordinal+1,0,0);
  assert(channel_has_pending_query(c,fd,2000));
  query_t q; assert(channel_read_query(c,fd,&q));
  channel_profile_context(c,pid,++*ordinal,q.tag,q.time);
  return q;
}
static void answer(channel_t *c,int fd,enum answer tag)
{
  answer_t a={.tag=tag};
  if(tag==A_SIZE) a.size.size=7;
  if(tag==A_MTIM) a.mtim.mtime=9;
  channel_write_answer(c,fd,&a);channel_flush(c,fd);
}
static void bounded_records(void)
{
  if(!txp_activity_enabled()) return;
  txp_activity a;txp_activity_init(&a,"test-bound");
  txp_activity_sample s=txp_activity_clock(),e=s;
  for(int i=0;i<10;i++) {
    e.wall=s.wall+20+i;e.cpu=s.cpu+1;
    txp_activity_add(&a,TXPA_WORK,s,e,i,PACK('T','E','S','T'),i,0);
  }
  txp_activity_flush(&a,e.wall);
}
int main(void)
{
  alarm(10);int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
  int small=4096;assert(setsockopt(pair[1],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
  pid_t worker=fork();assert(worker>=0);
  if(!worker) {close(pair[0]);activity_test_worker(pair[1]);}
  close(pair[1]);channel_t *c=channel_new();channel_handshake_begin(c);
  int status;do {status=channel_handshake_poll(c,pair[0]);if(!status) usleep(1000);} while(!status);
  assert(status==CHANNEL_HANDSHAKE_READY);uint64_t ordinal=0;
  query_t q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_SIZE && q.time==101);answer(c,pair[0],A_SIZE);
  q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_MTIM && q.time==202);answer(c,pair[0],A_MTIM);
  q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_READ && q.time==303);
  // Send the tag first. The subsequent payload read, not tag read, must own
  // this delay. The real worker's transaction must span both reads.
  assert(write(pair[0],"READ",4)==4);usleep(70000);
  uint32_t one=1;assert(write(pair[0],&one,4)==4);assert(write(pair[0],"x",1)==1);
  q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_SEEN);
  q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_CHLD && ordinal==5);
  int child=q.chld.pid,childfd=q.chld.fd;uint64_t childordinal=0;
  channel_t *cc=channel_new();
  q=next(cc,childfd,child,&childordinal);assert(q.tag==Q_SIZE && q.time==505);answer(cc,childfd,A_SIZE);
  q=next(cc,childfd,child,&childordinal);assert(q.tag==Q_FINI);answer(cc,childfd,A_DONE);
  close(childfd);channel_free(cc);
  // The parent can only return after the child exits AND DONE is received.
  usleep(50000);channel_profile_context(c,worker,ordinal,Q_CHLD,303);answer(c,pair[0],A_DONE);
  q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_SIZE && q.time==606 && ordinal==6);answer(c,pair[0],A_SIZE);
  q=next(c,pair[0],worker,&ordinal);assert(q.tag==Q_FINI && ordinal==7);answer(c,pair[0],A_DONE);
  close(pair[0]);channel_free(c);assert(waitpid(worker,&status,0)==worker && WIFEXITED(status) && WEXITSTATUS(status)==0);
  bounded_records();puts("activity protocol: 7 parent queries, 2 child queries");
}

#ifndef TXP_ACTIVITY_H
#define TXP_ACTIVITY_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Diagnostic clocks and output are entirely opt-in. CPU always means this
 * process's own CPU; wall minus CPU also includes subprocess and external I/O. */
enum txp_activity_kind { TXPA_WORK, TXPA_READ, TXPA_WRITE, TXPA_FLUSH,
  TXPA_TRANSACTION, TXPA_POLL, TXPA_QUERY_READ, TXPA_ANSWER, TXPA_RESPONSE_FLUSH,
  TXPA_SOCKET_READ, TXPA_SOCKET_WRITE, TXPA_FORK, TXPA_PARK, TXPA_IDLE, TXPA_INPUT, TXPA_LOOP, TXPA_COUNT };
typedef struct { double wall, cpu; pid_t pid; } txp_activity_sample;
typedef struct {
  uint64_t count, query; double wall, cpu, max_wall, max_cpu, max_wall_cpu, max_at;
  int tag, tokens, peer;
} txp_activity_totals;
typedef struct txp_activity {
  const char *actor;
  pid_t pid;
  double window;
  unsigned slow, dropped;
  txp_activity_totals totals[TXPA_COUNT];
} txp_activity;

static inline int txp_activity_enabled(void)
{
  static int enabled=-1;
  if (enabled<0) {
    const char *v=getenv("TEXPRESSO_PROFILE"), *detail=getenv("TEXPRESSO_PROFILE_ACTIVITY");
    enabled=v && v[0]=='1' && detail && detail[0]=='1';
  }
  return enabled;
}
static inline txp_activity_sample txp_activity_clock(void)
{
  struct timespec wall, cpu;
  clock_gettime(CLOCK_MONOTONIC,&wall);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu);
  return (txp_activity_sample){wall.tv_sec*1000.0+wall.tv_nsec/1000000.0,
    cpu.tv_sec*1000.0+cpu.tv_nsec/1000000.0,getpid()};
}
static inline const char *txp_activity_name(int kind)
{
  static const char *names[]={"work","read","write","flush","transaction","poll",
    "query-read","answer","response-flush","socket-read","socket-write","fork","park","idle","input","loop"};
  return names[kind];
}
static inline void txp_activity_tag(int tag,char out[5])
{
  for(int i=0;i<4;++i) { unsigned char c=(unsigned)tag>>(8*i); out[i]=c>='A'&&c<='Z'?c:'?'; }
  out[4]=0;
}
/* One bounded write per JSON line avoids interleaving between forked workers.
 * 512 bytes fits POSIX's minimum PIPE_BUF, including the profiling prefix. */
static inline void txp_activity_write(txp_activity *a,const char *line,int length)
{
  if(length<=0 || length>=512 || write(STDERR_FILENO,line,(size_t)length)!=length) a->dropped++;
}
static inline void txp_activity_init(txp_activity *a,const char *actor)
{
  memset(a,0,sizeof(*a)); a->actor=actor; a->pid=getpid();
}
static inline void txp_activity_flush(txp_activity *a,double now)
{
  if(!txp_activity_enabled() || !a->window) return;
  unsigned reported_drops=a->dropped;
  for(int i=0;i<TXPA_COUNT;++i) {
    txp_activity_totals *t=&a->totals[i]; if(!t->count) continue;
    char tag[5],line[512]; txp_activity_tag(t->tag,tag);
    int n=snprintf(line,sizeof(line),"[profile] {\"stage\":\"native-activity\",\"actor\":\"%s\",\"kind\":\"%s\",\"pid\":%d,\"nativeAt\":%.4f,\"windowMs\":%.3f,\"count\":%llu,\"ms\":%.4f,\"cpuMs\":%.4f,\"maxMs\":%.4f,\"maxCpuMs\":%.4f,\"cpuAtMaxMs\":%.4f,\"maxAt\":%.4f,\"query\":%llu,\"tag\":\"%s\",\"tokens\":%d,\"peerPid\":%d,\"dropped\":%u}\n",
      a->actor,txp_activity_name(i),(int)a->pid,now,now-a->window,
      (unsigned long long)t->count,t->wall,t->cpu,t->max_wall,t->max_cpu,t->max_wall_cpu,t->max_at,
      (unsigned long long)t->query,tag,t->tokens,t->peer,a->dropped);
    txp_activity_write(a,line,n);
  }
  memset(a->totals,0,sizeof(a->totals)); a->window=now; a->slow=0; a->dropped-=reported_drops;
}
static inline void txp_activity_add(txp_activity *a,int kind,txp_activity_sample start,
  txp_activity_sample end,uint64_t query,int tag,int tokens,int peer)
{
  if(!txp_activity_enabled() || start.pid!=end.pid || end.cpu<start.cpu) return;
  if(a->pid!=end.pid) txp_activity_init(a,a->actor);
  if(!a->window) a->window=start.wall;
  double wall=end.wall-start.wall,cpu=end.cpu-start.cpu;
  txp_activity_totals *t=&a->totals[kind]; t->count++;t->wall+=wall;t->cpu+=cpu;
  if(wall>=t->max_wall) {t->max_wall=wall;t->max_wall_cpu=cpu;t->max_at=end.wall;t->query=query;t->tag=tag;t->tokens=tokens;t->peer=peer;}
  if(cpu>t->max_cpu) t->max_cpu=cpu;
  if(wall>=10) {
    if(a->slow<4) {
      char label[5],line[512];txp_activity_tag(tag,label);a->slow++;
      int n=snprintf(line,sizeof(line),"[profile] {\"stage\":\"native-slow\",\"actor\":\"%s\",\"kind\":\"%s\",\"pid\":%d,\"nativeAt\":%.4f,\"beginAt\":%.4f,\"ms\":%.4f,\"cpuMs\":%.4f,\"query\":%llu,\"tag\":\"%s\",\"tokens\":%d,\"peerPid\":%d}\n",
        a->actor,txp_activity_name(kind),(int)end.pid,end.wall,start.wall,wall,cpu,
        (unsigned long long)query,label,tokens,peer);
      txp_activity_write(a,line,n);
    } else a->dropped++;
  }
  if(end.wall-a->window>=250) txp_activity_flush(a,end.wall);
}
#endif

/* Frontend side of the worker channel when the worker misbehaves or vanishes.
 * Each case once aborted the whole frontend. */
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "../src/frontend/sprotocol.h"

// driver.c provides this in the real binaries.
void print_backtrace(void) { }

static void pair(int fds[2])
{
  assert(socketpair(PF_UNIX, SOCK_STREAM, 0, fds) == 0);
}

static void send_bytes(int fd, const void *data, size_t len)
{
  assert(write(fd, data, len) == (ssize_t)len);
}

// Same framing as send_child_fd in src/engine/main/fork.c, optionally preceded
// by one-way messages coalesced into the same segment.
static int send_chld(int fd, const void *prefix, size_t prefix_len, int32_t pid)
{
  int child[2];
  pair(child);
  uint32_t time = 7;
  char control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov[4] = {
    { (void *)prefix, prefix_len },
    { "CHLD", 4 }, { &time, 4 }, { &pid, 4 },
  };
  struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 4,
                        .msg_control = control, .msg_controllen = sizeof(control) };
  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &child[0], sizeof(int));
  assert(sendmsg(fd, &msg, 0) == (ssize_t)(prefix_len + 12));
  close(child[0]);
  return child[1];
}

// SEEN: tag, time, fid, pos.
static void seen(char out[16], uint32_t fid, uint32_t pos)
{
  uint32_t time = 5;
  memcpy(out, "SEEN", 4);
  memcpy(out + 4, &time, 4);
  memcpy(out + 8, &fid, 4);
  memcpy(out + 12, &pos, 4);
}

static bool is_closed(int fd)
{
  struct pollfd p = { .fd = fd, .events = POLLIN };
  char byte;
  return poll(&p, 1, 1000) == 1 && read(fd, &byte, 1) == 0;
}

// An edit pops the worker while a CHLD (with its descriptor) is still buffered
// behind a one-way SEEN. The next worker's fork must still be accepted, and the
// orphaned checkpoint's socket must be closed so that it can exit.
static void test_reset_discards_unparsed_descriptor(void)
{
  channel_t *c = channel_new();
  int first[2], second[2];
  pair(first);
  pair(second);

  char prefix[16];
  seen(prefix, 3, 10);
  int orphan = send_chld(first[1], prefix, sizeof prefix, 101);
  query_t q;
  assert(channel_read_query(c, first[0], &q) && q.tag == Q_SEEN);
  channel_reset(c);

  int kept = send_chld(second[1], NULL, 0, 202);
  assert(channel_read_query(c, second[0], &q));
  assert(is_closed(orphan));
  assert(q.tag == Q_CHLD && q.chld.pid == 202 && q.chld.fd >= 0);
  send_bytes(kept, "x", 1);
  char byte;
  assert(read(q.chld.fd, &byte, 1) == 1 && byte == 'x');

  close(q.chld.fd); close(kept); close(orphan);
  close(first[0]); close(first[1]); close(second[0]); close(second[1]);
  channel_free(c);
}

// A worker that exited after a TeX fatal error leaves EOF pending. Rollback
// peeks for SEEN messages at exactly that moment.
static void test_peek_at_eof(void)
{
  channel_t *c = channel_new();
  int fds[2];
  pair(fds);
  close(fds[1]);
  assert(channel_peek_query(c, fds[0]) == 0);
  query_t q;
  assert(!channel_read_query(c, fds[0], &q));
  close(fds[0]);
  channel_free(c);
}

// Peeking must not consume the message.
static void test_peek_keeps_query(void)
{
  channel_t *c = channel_new();
  int fds[2];
  pair(fds);
  char msg[16];
  seen(msg, 1, 2);
  send_bytes(fds[1], msg, sizeof msg);
  assert(channel_peek_query(c, fds[0]) == Q_SEEN);
  query_t q;
  assert(channel_read_query(c, fds[0], &q));
  assert(q.tag == Q_SEEN && q.seen.fid == 1 && q.seen.pos == 2);
  close(fds[0]); close(fds[1]);
  channel_free(c);
}

// A worker killed in the middle of a message must not yield a query made of
// zero-filled fields.
static void test_truncated_queries(void)
{
  static const struct { const char *bytes; size_t len; } cases[] = {
    { "READ\x01\0\0\0\x02\0\0\0", 12 },          // fid, missing pos/size
    { "OPRD\0\0\0\0\0\0\0\0main.t", 18 },        // unterminated path
    { "APND\0\0\0\0\0\0\0\0\x10\0\0\0abc", 19 }, // short payload
    { "SEEN\0\0", 6 },                           // short time
  };
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
  {
    channel_t *c = channel_new();
    int fds[2];
    pair(fds);
    send_bytes(fds[1], cases[i].bytes, cases[i].len);
    close(fds[1]);
    query_t q;
    assert(!channel_read_query(c, fds[0], &q));
    close(fds[0]);
    channel_free(c);
  }
}

static void test_protocol_violations(void)
{
  static const struct { const char *bytes; size_t len; } cases[] = {
    { "JUNK\0\0\0\0", 8 },                          // unknown tag
    { "CHLD\0\0\0\0\x05\0\0\0", 12 },               // CHLD without descriptor
    { "APND\0\0\0\0\0\0\0\0\xff\xff\xff\x7f", 16 }, // absurd payload size
    { "APND\0\0\0\0\0\0\0\0\xff\xff\xff\xff", 16 }, // negative payload size
  };
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
  {
    channel_t *c = channel_new();
    int fds[2];
    pair(fds);
    send_bytes(fds[1], cases[i].bytes, cases[i].len);
    query_t q;
    assert(!channel_read_query(c, fds[0], &q));
    close(fds[0]); close(fds[1]);
    channel_free(c);
  }
}

int main(void)
{
  test_reset_discards_unparsed_descriptor();
  test_peek_at_eof();
  test_peek_keeps_query();
  test_truncated_queries();
  test_protocol_violations();
  puts("PASS: sprotocol");
  return 0;
}

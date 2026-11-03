/* The frontend's reader for messages from compiler workers (sprotocol.c).
 *
 * The input is a sequence of socket writes, each optionally carrying a
 * descriptor the way CHLD does, followed by reader actions: read a query,
 * peek, or reset the channel as an edit does when it pops a worker. The worker
 * end is then closed, as when a worker exits.
 *
 * Oracle: the reader never aborts, every descriptor handed to the channel is
 * eventually returned in a CHLD query or closed, and reading stops cleanly at
 * EOF. */
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include "common.h"
#include "sprotocol.h"

#define MAX_FDS 8

static bool peer_closed(int fd)
{
  struct pollfd p = { .fd = fd, .events = POLLIN };
  char byte;
  return poll(&p, 1, 1000) == 1 && recv(fd, &byte, 1, MSG_DONTWAIT) == 0;
}

static void send_chunk(int fd, const uint8_t *data, size_t len, int pass)
{
  char control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = { (void *)data, len };
  struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
  if (pass >= 0)
  {
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &pass, sizeof(int));
  }
  fuzz_check(sendmsg(fd, &msg, 0) == (ssize_t)len);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size > 32768)
    return 0;
  fuzz_input in = { data, size, 0 };
  int fds[2];
  fuzz_check(socketpair(PF_UNIX, SOCK_STREAM, 0, fds) == 0);
  int bufsize = 256 * 1024;
  setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof bufsize);
  setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof bufsize);

  // Writes. A descriptor needs at least one byte of data to travel with.
  int kept[MAX_FDS], nkept = 0;
  int chunks = fuzz_u8(&in) % 16;
  for (int i = 0; i < chunks && fuzz_more(&in); i++)
  {
    uint8_t flags = fuzz_u8(&in);
    const uint8_t *bytes;
    size_t len = fuzz_bytes(&in, fuzz_u16(&in) % 8192, &bytes);
    int pass = -1;
    if ((flags & 1) && len > 0 && nkept < MAX_FDS)
    {
      int child[2];
      fuzz_check(socketpair(PF_UNIX, SOCK_STREAM, 0, child) == 0);
      pass = child[0];
      kept[nkept++] = child[1];
    }
    if (len > 0)
      send_chunk(fds[1], bytes, len, pass);
    if (pass >= 0)
      close(pass);
  }
  close(fds[1]);

  channel_t *c = channel_new();
  for (int i = 0; i < 64; i++)
  {
    query_t q;
    uint8_t action = fuzz_u8(&in) % 8;
    if (action == 0)
    {
      (void)channel_peek_query(c, fds[0]);
      continue;
    }
    if (action == 1)
    {
      channel_reset(c);
      continue;
    }
    if (!channel_read_query(c, fds[0], &q))
      break;
    if (q.tag == Q_CHLD)
    {
      fuzz_check(q.chld.fd >= 0);
      close(q.chld.fd);
    }
    if (q.tag == Q_APND)
      fuzz_check(q.apnd.size >= 0);
  }
  channel_free(c);
  close(fds[0]);

  for (int i = 0; i < nkept; i++)
  {
    // Descriptors still queued in the closed socket are released by the
    // kernel; any the channel received must have been closed by now.
    fuzz_check(peer_closed(kept[i]));
    close(kept[i]);
  }
  return 0;
}

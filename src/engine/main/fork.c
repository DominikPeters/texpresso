#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include "../../include/txp_activity.h"

#define NO_EINTR(command) \
  do {} while ((command) == -1 && errno == EINTR)

#define STR(X) #X
#define SSTR(X) STR(X)

#define PERROR(result) \
  if ((result) == -1)             \
  {                           \
    perror("texpresso_fork_with_channel failure (" __FILE__ ":" SSTR(__LINE__) ")"); \
    abort();                  \
  }

#define PASSERT(command, ...) \
  if (!(command))             \
  {                           \
  fprintf(stderr, "texpresso_fork_with_channel failure (" #command ") " __VA_ARGS__); \
    abort();                  \
  }

static void send_child_fd(int chan_fd, int32_t pid, uint32_t time, int child_fd)
{
  ssize_t sent;
  char msg_control[CMSG_SPACE(1 * sizeof(int))] = {0,};
  struct iovec iov[3] = {
    { .iov_base = "CHLD", .iov_len = 4 },
    { .iov_base = &time, .iov_len = 4 },
    { .iov_base = &pid, .iov_len = 4 },
  };
  struct msghdr msg = {
    .msg_iov = iov, .msg_iovlen = 3,
    .msg_controllen = CMSG_SPACE(1 * sizeof(int)),
  };
  msg.msg_control = &msg_control;

  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(1 * sizeof(int));

  int *fds0 = (int*)CMSG_DATA(cm);
  fds0[0] = child_fd;

  NO_EINTR(sent = sendmsg(chan_fd, &msg, 0));
  PERROR(sent);
  PASSERT(sent == 12);
}

pid_t texpresso_fork_with_channel(int fd, uint32_t time, txp_activity *profile, uint64_t query)
{
  int sockets[2];

  // Create socket
  PERROR(socketpair(PF_UNIX, SOCK_STREAM, 0, sockets));

  // Fork
  pid_t child;
  txp_activity_sample fork_start={0},park_start={0};
  if(profile) fork_start=txp_activity_clock();
  PERROR((child = fork()));
  if(profile && child>0) {
    txp_activity_add(profile,TXPA_FORK,fork_start,txp_activity_clock(),query,0x444c4843,time,child);
    park_start=txp_activity_clock();
  }

  if (child == 0)
  {
    // In child: replace channel with new socket
    PERROR(dup2(sockets[1], fd));
    PERROR(close(sockets[0]));
    PERROR(close(sockets[1]));
  }
  else
  {
    // In parent: send other end of new socket to driver
    send_child_fd(fd, child, time, sockets[0]);

    // The driver owns the transferred endpoint, and the child owns the other
    // one. Keeping either here while parked prevents EOF from reaching the
    // child when the driver exits, stranding this entire checkpoint chain.
    PERROR(close(sockets[0]));
    PERROR(close(sockets[1]));

    // Wait for process to end
    int status;
    while (waitpid(child, &status, 0) == -1)
    {
      if (errno == EINTR)
        continue;
      perror("waitpid");
      return 1;
    }

    // Resume handling
    char answer[4];
    int recvd;
    do {
      NO_EINTR(recvd = read(fd, answer, 4));

      // Ignore any flush message, the buffers have been flushed
      // anyway before starting the fork.
    } while (recvd == 4 &&
             answer[0] == 'F' && answer[1] == 'L' &&
             answer[2] == 'S' && answer[3] == 'H');

    // A closed driver is normal session teardown. Unwind parked ancestors
    // without an abort/backtrace or any signal from the departed frontend.
    if (recvd == 0 || (recvd < 0 && errno == ECONNRESET))
      _exit(0);

    if(profile) {
      txp_activity_add(profile,TXPA_PARK,park_start,txp_activity_clock(),query,0x444c4843,time,child);
      txp_activity_flush(profile,txp_activity_clock().wall);
    }
    PASSERT(recvd == 4 &&
            answer[0] == 'D' && answer[1] == 'O' &&
            answer[2] == 'N' && answer[3] == 'E',
            "recvd: %d, answer: %c%c%c%c",
            recvd,
            answer[0], answer[1], answer[2], answer[3]);
  }
  return child;
}

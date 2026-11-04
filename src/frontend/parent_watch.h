#ifndef TXP_PARENT_WATCH_H
#define TXP_PARENT_WATCH_H

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Opt-in protection for an editor-owned, isolated process group. Start before
 * SDL, threads, document loading or compiler checkpoints allocate any state.
 * The tiny original process watches its launching editor and the real
 * frontend. A crash/force-quit must not strand forked compiler snapshots.
 * Returns -1 in the frontend, or an error status if protection cannot start.
 * The supervisor exits with the group; clients already treat an unsolicited
 * frontend exit (including exit 0) as a stopped/failed live session.
 */
static int txp_watch_parent(void)
{
    const pid_t group = getpid(), parent = getppid();
    if (getpgrp() != group || parent <= 1) {
        fprintf(stderr, "[error] -watch-parent requires a live parent and an isolated process group\n");
        return 1;
    }
    const pid_t frontend = fork();
    if (frontend < 0) {
        perror("[error] starting parent watcher");
        return 1;
    }
    if (!frontend) return -1;

    for (;;) {
        int status;
        const pid_t result = waitpid(frontend, &status, WNOHANG);
        if (result == frontend || getppid() != parent || (result < 0 && errno != EINTR)) {
            fprintf(stderr, "[process] Releasing compiler group: %s\n",
                    result == frontend ? "frontend exited" : "launching editor exited");
            /* The group was created by the client specifically for this
             * session. Include the watcher so it cannot become an orphan. */
            kill(-group, SIGKILL);
            _exit(1);
        }
        const struct timespec interval = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&interval, NULL);
    }
}
#endif

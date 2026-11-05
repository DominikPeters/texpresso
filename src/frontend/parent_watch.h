#ifndef TXP_PARENT_WATCH_H
#define TXP_PARENT_WATCH_H

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t txp_parent_watch_stopping;
static void txp_parent_watch_stop(int signal_number)
{
    (void)signal_number;
    txp_parent_watch_stopping = 1;
}

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
    // A TERM sent to just the supervisor must release the group too, rather
    // than killing the only process still watching its lifetime. Install
    // before fork so even a shutdown during startup cannot miss the handler.
    const int signals[] = { SIGTERM, SIGINT, SIGHUP };
    struct sigaction previous[3], stop_action = {0};
    stop_action.sa_handler = txp_parent_watch_stop;
    sigemptyset(&stop_action.sa_mask);
    for (int i = 0; i < 3; ++i) {
        if (sigaction(signals[i], &stop_action, &previous[i]) < 0) {
            perror("[error] installing parent watcher signal handler");
            return 1;
        }
    }
    const pid_t frontend = fork();
    if (frontend < 0) {
        perror("[error] starting parent watcher");
        return 1;
    }
    if (!frontend) {
        for (int i = 0; i < 3; ++i) sigaction(signals[i], &previous[i], NULL);
        return -1;
    }

    for (;;) {
        int status;
        const pid_t result = waitpid(frontend, &status, WNOHANG);
        if (txp_parent_watch_stopping || result == frontend || getppid() != parent || (result < 0 && errno != EINTR)) {
            fprintf(stderr, "[process] Releasing compiler group: %s\n",
                    txp_parent_watch_stopping ? "supervisor stopped" : result == frontend ? "frontend exited" : "launching editor exited");
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

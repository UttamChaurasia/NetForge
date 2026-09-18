/*
 * daemon.c — NetForge daemonization (Phase 7)
 *
 * Reference: W. Richard Stevens, "Advanced Programming in the UNIX
 * Environment", §13.3 — the canonical description of the double-fork daemon.
 */

#include "daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int daemonize(void)
{
    pid_t pid;

    /* --- Fork #1 --------------------------------------------------------
     * The parent exits immediately so the shell gets its prompt back and
     * the child is guaranteed not to be a process group leader (required
     * for setsid() to succeed).
     * ------------------------------------------------------------------- */
    pid = fork();
    if (pid < 0)  return -1;   /* fork failed */
    if (pid > 0)  _exit(0);    /* parent exits — child continues */

    /* --- setsid() -------------------------------------------------------
     * Create a new session. The calling process becomes the session leader
     * AND the process group leader of a new process group. As session
     * leader it COULD open a controlling terminal (on SVR4/BSDs; not Linux,
     * but we don't rely on that). Fork again to prevent that.
     * ------------------------------------------------------------------- */
    if (setsid() < 0) return -1;

    /* --- Fork #2 --------------------------------------------------------
     * The grandchild is NOT a session leader, so it can never acquire a
     * controlling tty. This is the point of the double-fork.
     * ------------------------------------------------------------------- */
    pid = fork();
    if (pid < 0)  return -1;
    if (pid > 0)  _exit(0);    /* second parent exits */

    /* --- Housekeeping ---------------------------------------------------
     * chdir to root: prevents holding any mount point open.
     * umask to 0: give the daemon full control over file creation modes.
     * ------------------------------------------------------------------- */
    if (chdir("/") < 0) return -1;
    umask(0);

    /* --- Redirect standard file descriptors ----------------------------
     * Close and reopen as /dev/null so that any accidental printf() or
     * read from stdin doesn't produce confusing output or block.
     * ------------------------------------------------------------------- */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) return -1;

    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);

    if (devnull > STDERR_FILENO)
        close(devnull);

    return 0;
}

int write_pidfile(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return 0;
}

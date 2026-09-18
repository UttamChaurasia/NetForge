/*
 * daemon.h — NetForge daemonization helpers (Phase 7)
 */

#ifndef NETFORGE_DAEMON_H
#define NETFORGE_DAEMON_H

/*
 * daemonize — detach from the controlling terminal and become a background
 * daemon. Call this early in main(), before opening sockets.
 *
 * What it does:
 *   1. fork() — parent exits; child is adopted by init (PID 1).
 *   2. setsid() — child becomes session leader (no controlling tty).
 *   3. fork() again — grandchild can never re-acquire a tty (session leaders
 *      can; a non-leader cannot). This is the classic "double-fork" trick.
 *   4. chdir("/") — so the daemon doesn't hold a mount point open.
 *   5. Redirect stdin/stdout/stderr → /dev/null.
 *
 * Returns 0 on success (in the daemon grandchild).
 * Returns -1 on error (errno set by the failing syscall).
 *
 * The calling process (first fork parent) never returns from daemonize —
 * it exits via _exit() after the first fork.
 */
int daemonize(void);

/*
 * write_pidfile — write the daemon's PID to path.
 *
 * Useful so other processes (e.g. an init script) can send signals by
 * reading kill $(cat /var/run/netforge.pid).
 *
 * Returns 0 on success, -1 on error.
 */
int write_pidfile(const char *path);

#endif /* NETFORGE_DAEMON_H */

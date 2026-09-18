/*
 * server.c — NetForge pthreads TCP server
 *
 * PHASES COVERED
 * --------------
 * Phase 1 — Single-client TCP echo (socket/bind/listen/accept/read/write)
 * Phase 2 — Multi-client via fork() [see fork_handle_client, search PHASE_2]
 * Phase 3 — Multi-client via pthreads + mutex-protected client list
 * Phase 4 — Custom length-prefixed protocol (send_message/recv_message)
 * Phase 5 — Broadcast: every message forwarded to all other clients
 * Phase 6 — Graceful shutdown on SIGINT (shutdown_flag, close all fds)
 * Phase 7 — Optional daemonization (--daemon flag)
 *
 * BUILD
 * -----
 *   make server          # debug build
 *   make release server  # optimised build
 *
 * RUN
 * ---
 *   ./build/server [--port PORT] [--daemon]
 *   ./build/server --port 9090
 *
 * CONNECT
 * -------
 *   ./build/client --port 9090
 *   # or: nc localhost 9090  (Phase 1 echo only; nc doesn't speak our framing)
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "protocol.h"
#include "daemon.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

#define DEFAULT_PORT      9090
#define MAX_CLIENTS       128
#define RECV_BUF_SIZE     (MSG_MAX_PAYLOAD + 1)
#define BACKLOG           16        /* listen() queue depth */
#define PIDFILE           "/tmp/netforge.pid"

/* =========================================================================
 * Client record
 * ========================================================================= */

typedef struct {
    int          fd;          /* -1 means slot is free */
    pthread_t    thread;
    struct sockaddr_in addr;
} client_t;

/* =========================================================================
 * Globals (shared across threads — all access must be under the mutex)
 * ========================================================================= */

static client_t        clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * shutdown_flag — written only by the SIGINT handler, read by the main loop.
 * Must be volatile sig_atomic_t so the compiler doesn't cache it in a register
 * and the signal handler's write is visible to the main thread without
 * requiring a full memory barrier (sig_atomic_t reads/writes are atomic on
 * the target platform by definition).
 */
static volatile sig_atomic_t shutdown_flag = 0;

static int listen_fd = -1;   /* global so signal handler can close it */
static int mode_echo = 0;    /* 1: echo back to sender; 0: broadcast to others */

/* =========================================================================
 * PHASE 6 — Signal handling
 * ========================================================================= */

static void sigint_handler(int sig)
{
    (void)sig;          /* suppress unused-parameter warning */
    shutdown_flag = 1;  /* tell the main loop to stop accepting */

    /*
     * We close the listening fd from the signal handler to break the
     * accept() call that is blocking in the main thread. accept() will
     * return -1 with errno == EBADF or EINVAL, which the main loop treats
     * as a shutdown signal.
     *
     * Note: close() is async-signal-safe per POSIX.
     */
    if (listen_fd != -1) {
        close(listen_fd);
        listen_fd = -1;
    }
}

static void sigchld_handler(int sig)
{
    /*
     * PHASE 2 — Reap zombie child processes.
     *
     * Without this, every child process that exits becomes a zombie (it
     * stays in the process table until the parent calls wait()). The
     * WNOHANG flag prevents blocking — we just harvest whatever children
     * have already exited and return immediately.
     */
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void setup_signals(void)
{
    struct sigaction sa_int, sa_chld;

    memset(&sa_int,  0, sizeof sa_int);
    memset(&sa_chld, 0, sizeof sa_chld);

    sa_int.sa_handler  = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;   /* no SA_RESTART — we want accept() to fail */
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);

    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    /* Ignore SIGPIPE — we handle write() errors explicitly.
     * Without this, writing to a disconnected client would kill the server. */
    signal(SIGPIPE, SIG_IGN);
}

/* =========================================================================
 * Client list helpers (always call under clients_lock)
 * ========================================================================= */

static int clients_add(int fd, pthread_t thread, struct sockaddr_in *addr)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == -1) {
            clients[i].fd     = fd;
            clients[i].thread = thread;
            clients[i].addr   = *addr;
            return i;
        }
    }
    return -1;   /* no free slot */
}

static void clients_remove(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            clients[i].fd = -1;
            return;
        }
    }
}

/* =========================================================================
 * PHASE 5 — Broadcast
 *
 * Send msg to every connected client except sender_fd. Called from a client
 * thread; we hold the lock for the entire iteration so the list doesn't
 * change under us. A write() failure means the remote peer has gone away —
 * we just skip it; its own thread will detect the read failure and clean up.
 * ========================================================================= */

static void broadcast(int sender_fd, const char *msg, uint32_t len)
{
    pthread_mutex_lock(&clients_lock);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = clients[i].fd;
        if (fd == -1 || fd == sender_fd) continue;

        if (send_message(fd, msg, len) < 0) {
            /* Client probably disconnected. Its thread will clean up the slot.
             * We do NOT call clients_remove() here to avoid a deadlock (we
             * already hold the lock; clients_remove also wants the lock). */
            fprintf(stderr, "[server] broadcast write to fd=%d failed: %s\n",
                    fd, strerror(errno));
        }
    }

    pthread_mutex_unlock(&clients_lock);
}

/* =========================================================================
 * PHASE 3 — Per-client thread function
 *
 * Each connected client runs this function in its own pthread. The thread
 * runs until:
 *   - The client disconnects (recv_message returns 1 / EOF)
 *   - A read error occurs
 *   - The server shuts down (shutdown_flag, handled indirectly via close)
 * ========================================================================= */

typedef struct {
    int              fd;
    struct sockaddr_in addr;
} thread_arg_t;

static void *handle_client(void *arg)
{
    thread_arg_t *ta = (thread_arg_t *)arg;
    int  fd   = ta->fd;
    char peer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ta->addr.sin_addr, peer, sizeof peer);
    uint16_t port = ntohs(ta->addr.sin_port);
    free(ta);

    printf("[server] client connected: %s:%u (fd=%d)\n", peer, port, fd);

    char   *buf = malloc(RECV_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "[server] malloc failed\n");
        close(fd);
        return NULL;
    }

    while (!shutdown_flag) {
        uint32_t len = 0;
        int rc = recv_message(fd, buf, RECV_BUF_SIZE - 1, &len);

        if (rc == 1) {
            /* Clean EOF — client disconnected gracefully */
            printf("[server] client disconnected: %s:%u\n", peer, port);
            break;
        }
        if (rc != 0) {
            /* I/O error or oversized message */
            fprintf(stderr, "[server] recv_message error (fd=%d rc=%d): %s\n",
                    fd, rc, strerror(errno));
            break;
        }

        buf[len] = '\0';
        printf("[server] fd=%d says (%u bytes): %s\n", fd, len, buf);

        /*
         * PHASE 1/3 (echo) vs PHASE 5 (broadcast):
         * If --echo was passed, echo back to the sender.
         * Otherwise, broadcast to all other connected clients.
         */
        if (mode_echo) {
            send_message(fd, buf, len);
        } else {
            broadcast(fd, buf, len);
        }
    }

    free(buf);
    close(fd);

    pthread_mutex_lock(&clients_lock);
    clients_remove(fd);
    pthread_mutex_unlock(&clients_lock);

    printf("[server] thread for fd=%d exiting\n", fd);
    return NULL;
}

/* =========================================================================
 * PHASE 2 — Fork-based handler (kept alongside threads for comparison)
 *
 * Uncomment the fork path in main() to use this instead of threads.
 * It's here so you can read both approaches side-by-side.
 * ========================================================================= */

static void fork_handle_client(int client_fd)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        close(client_fd);
        return;
    }

    if (pid == 0) {
        /* ---- CHILD --------------------------------------------------------
         * After fork(), both parent and child have open copies of the same
         * file descriptors. The child doesn't need listen_fd; the parent
         * doesn't need client_fd. Both should close what they don't use —
         * otherwise the "extra" references keep the sockets alive longer
         * than intended and confuse connection teardown.
         * ----------------------------------------------------------------- */
        close(listen_fd);

        char *buf = malloc(RECV_BUF_SIZE);
        if (!buf) {
            close(client_fd);
            _exit(1);
        }

        uint32_t len = 0;
        while (recv_message(client_fd, buf, RECV_BUF_SIZE - 1, &len) == 0) {
            buf[len] = '\0';
            printf("[child pid=%d] fd=%d echoing (%u bytes): %s\n",
                   (int)getpid(), client_fd, len, buf);
            if (send_message(client_fd, buf, len) < 0)
                break;
        }

        free(buf);
        close(client_fd);
        _exit(0);   /* _exit, not exit — don't run atexit handlers in child */
    }

    /* ---- PARENT ----------------------------------------------------------
     * Parent closes the client fd (the child has its own copy).
     * The SIGCHLD handler will reap the zombie when the child exits.
     * -------------------------------------------------------------------- */
    close(client_fd);
}

/* =========================================================================
 * Listening socket setup (Phase 1)
 * ========================================================================= */

static int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /*
     * SO_REUSEADDR — allows us to bind to a port that is in TIME_WAIT state.
     * TIME_WAIT lasts up to 2×MSL (typically 60–120 s) after a connection
     * closes. Without this flag, every server restart during development
     * fails with "Address already in use" for a couple of minutes. Set it
     * unconditionally; it's always the right thing to do on a server socket.
     */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;    /* bind to all interfaces */
    addr.sin_port        = htons((uint16_t)port);
    /*
     * htons() converts the port number from host byte order to network byte
     * order (big-endian). On little-endian x86 machines this is a real byte
     * swap; on big-endian machines it's a no-op. Always use it — portability
     * and correctness on all architectures depends on it.
     */

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /*
     * listen() marks fd as a passive socket and sets the kernel's accept
     * backlog (number of pending connections that can queue up before the
     * kernel starts refusing new SYN packets with RST).
     */
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int  port       = DEFAULT_PORT;
    int  do_daemon  = 0;
    int  mode_fork  = 0;

    /* --- Parse arguments ------------------------------------------------ */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--daemon") == 0) {
            do_daemon = 1;
        } else if (strcmp(argv[i], "--fork") == 0) {
            mode_fork = 1;
        } else if (strcmp(argv[i], "--echo") == 0) {
            mode_echo = 1;
        } else {
            fprintf(stderr, "Usage: %s [--port PORT] [--daemon] [--fork] [--echo]\n", argv[0]);
            return 1;
        }
    }

    /* --- Phase 7: daemonize before opening sockets --------------------- */
    if (do_daemon) {
        if (daemonize() < 0) {
            perror("daemonize");
            return 1;
        }
        write_pidfile(PIDFILE);
        /* After daemonize(), printf → /dev/null. Use a log file in
         * production; for now we accept silent daemon operation. */
    }

    /* --- Initialise client table --------------------------------------- */
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    /* --- Set up signal handlers ---------------------------------------- */
    setup_signals();

    /* --- Create listening socket --------------------------------------- */
    listen_fd = create_listen_socket(port);
    if (listen_fd < 0) return 1;

    printf("[server] NetForge listening on port %d (pid=%d)\n",
           port, (int)getpid());

    /* =========================================================================
     * MAIN ACCEPT LOOP
     *
     * accept() blocks until a client connects. It returns a NEW file
     * descriptor for that specific connection — the original listen_fd
     * stays open so we can accept more clients. This is the core insight
     * of Phase 1: two distinct fds, two distinct roles.
     * ========================================================================= */
    while (!shutdown_flag) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof client_addr;

        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr, &addrlen);

        if (client_fd < 0) {
            if (shutdown_flag) break;            /* SIGINT closed listen_fd */
            if (errno == EINTR)  continue;       /* signal interrupted — retry */
            perror("accept");
            continue;
        }

        /* --- PHASE 2: process-based client handling via fork() -------- */
        if (mode_fork) {
            fork_handle_client(client_fd);
            continue;
        }

        /* --- PHASE 3: spawn a thread per client ------------------------ */
        thread_arg_t *ta = malloc(sizeof *ta);
        if (!ta) {
            fprintf(stderr, "[server] malloc failed, dropping client\n");
            close(client_fd);
            continue;
        }
        ta->fd   = client_fd;
        ta->addr = client_addr;

        pthread_t tid;
        int err = pthread_create(&tid, NULL, handle_client, ta);
        if (err != 0) {
            fprintf(stderr, "[server] pthread_create: %s\n", strerror(err));
            free(ta);
            close(client_fd);
            continue;
        }

        /*
         * pthread_detach() tells the pthread library to automatically
         * reclaim the thread's resources when it exits, so we don't need
         * to call pthread_join() on it. Without this, the thread's exit
         * state would accumulate in memory ("thread zombie") until joined.
         */
        pthread_detach(tid);

        /* Record the client. We pass tid 0 here because the thread is
         * already detached and we won't join it; we just need the fd. */
        pthread_mutex_lock(&clients_lock);
        if (clients_add(client_fd, tid, &client_addr) < 0) {
            fprintf(stderr, "[server] client table full, dropping connection\n");
            pthread_mutex_unlock(&clients_lock);
            close(client_fd);
            continue;
        }
        pthread_mutex_unlock(&clients_lock);
    }

    /* =========================================================================
     * PHASE 6 — Graceful shutdown
     *
     * SIGINT set shutdown_flag and closed listen_fd. Now close every
     * connected client fd. The client threads will see EOF / EBADF on
     * their next recv_message() call and exit naturally.
     * ========================================================================= */
    printf("\n[server] shutting down...\n");

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            shutdown(clients[i].fd, SHUT_RDWR);
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    }
    pthread_mutex_unlock(&clients_lock);

    /* Give detached threads a moment to notice and exit cleanly.
     * A production server would use pthread_cancel + pthread_join instead,
     * but a short sleep is sufficient for a development/learning server. */
    struct timespec ts = {0, 100 * 1000 * 1000L};  /* 100 ms */
    nanosleep(&ts, NULL);

    printf("[server] done. goodbye.\n");
    return 0;
}

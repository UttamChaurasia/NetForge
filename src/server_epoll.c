/*
 * server_epoll.c — NetForge epoll-based event loop server (Phase 9)
 *
 * This is the same broadcast chat as server.c but implemented with a single
 * thread using Linux epoll instead of one pthread per client.
 *
 * WHY EPOLL?
 * ----------
 * The pthreads server (server.c) creates one OS thread per client. Thread
 * creation is cheap, but not free: each thread needs a stack (~8 MB default),
 * a kernel task_struct, and a TLB/scheduler slot. At hundreds of connections
 * this becomes measurable; at tens of thousands it breaks down entirely.
 *
 * epoll is the Linux answer: a single thread watches an arbitrary number of
 * file descriptors and the kernel tells it exactly which ones are ready for
 * I/O. No thread-per-client overhead. This is how nginx, Redis, and most
 * high-performance network daemons actually work.
 *
 * LEVEL vs EDGE TRIGGERED
 * -----------------------
 * Level-triggered (default, EPOLLIN only):
 *   epoll_wait keeps returning an fd as ready as long as there is data to
 *   read. Safe and easy: if you don't read all the data in one pass, you'll
 *   get another chance next iteration.
 *
 * Edge-triggered (EPOLLIN | EPOLLET):
 *   epoll_wait only notifies ONCE per state transition (no data → some data).
 *   You MUST read/write in a loop until EAGAIN, or you'll miss data. More
 *   efficient but much easier to get wrong.
 *
 * We use level-triggered here. Learn it first; edge-triggered is a later
 * exercise.
 *
 * BUILD: make epoll_server
 * RUN:   ./build/epoll_server [--port PORT]
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

#define DEFAULT_PORT   9090
#define MAX_EVENTS     64
#define MAX_CLIENTS    1024
#define BACKLOG        16

/* Per-connection state — one of these per connected client fd */
typedef struct {
    int fd;
    struct sockaddr_in addr;
} conn_t;

static conn_t  *conns[MAX_CLIENTS];   /* indexed by fd */
static int      epoll_fd = -1;
static int      listen_fd = -1;
static volatile sig_atomic_t shutdown_flag = 0;
static int mode_echo = 0;

/* -------------------------------------------------------------------------
 * Signal handler — same pattern as server.c
 * ---------------------------------------------------------------------- */
static void sigint_handler(int sig)
{
    (void)sig;
    shutdown_flag = 1;
    if (listen_fd != -1) { close(listen_fd); listen_fd = -1; }
}

/* -------------------------------------------------------------------------
 * Add fd to epoll watch set
 * ---------------------------------------------------------------------- */
static int epoll_add(int efd, int fd)
{
    struct epoll_event ev;
    ev.events  = EPOLLIN;    /* level-triggered, notify when readable */
    ev.data.fd = fd;
    return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
}

/* -------------------------------------------------------------------------
 * Remove fd from epoll watch set and close it
 * ---------------------------------------------------------------------- */
static void remove_client(int efd, int fd)
{
    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);

    if (fd >= 0 && fd < MAX_CLIENTS && conns[fd]) {
        char peer[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &conns[fd]->addr.sin_addr, peer, sizeof peer);
        printf("[epoll] client disconnected: %s (fd=%d)\n", peer, fd);
        free(conns[fd]);
        conns[fd] = NULL;
    }

    close(fd);
}

/* -------------------------------------------------------------------------
 * Broadcast msg to every connected client except sender_fd
 * ---------------------------------------------------------------------- */
static void broadcast(int sender_fd, const char *msg, uint32_t len)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!conns[i] || conns[i]->fd == sender_fd) continue;
        if (send_message(conns[i]->fd, msg, len) < 0) {
            /* The write failed — client is gone. Remove it now.
             * Safe to call remove_client from within the loop because we
             * use the index i, not a pointer that could be freed. */
            fprintf(stderr, "[epoll] broadcast to fd=%d failed, removing\n", i);
            remove_client(epoll_fd, i);
        }
    }
}

/* -------------------------------------------------------------------------
 * Accept a new connection and register it with epoll
 * ---------------------------------------------------------------------- */
static void accept_client(int efd, int lfd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;

    int fd = accept(lfd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN)
            perror("accept");
        return;
    }

    if (fd >= MAX_CLIENTS) {
        fprintf(stderr, "[epoll] fd %d exceeds MAX_CLIENTS, dropping\n", fd);
        close(fd);
        return;
    }

    conn_t *c = malloc(sizeof *c);
    if (!c) { close(fd); return; }
    c->fd   = fd;
    c->addr = addr;
    conns[fd] = c;

    epoll_add(efd, fd);

    char peer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, peer, sizeof peer);
    printf("[epoll] client connected: %s:%u (fd=%d)\n",
           peer, ntohs(addr.sin_port), fd);
}

/* -------------------------------------------------------------------------
 * Read and dispatch one message from a client fd
 * ---------------------------------------------------------------------- */
static void handle_readable(int efd, int fd)
{
    char    *buf = malloc(MSG_MAX_PAYLOAD + 1);
    if (!buf) return;

    uint32_t len = 0;
    int rc = recv_message(fd, buf, MSG_MAX_PAYLOAD, &len);

    if (rc != 0) {
        /* EOF or error — remove client */
        remove_client(efd, fd);
    } else {
        buf[len] = '\0';
        printf("[epoll] fd=%d (%u bytes): %s\n", fd, len, buf);
        if (mode_echo) {
            send_message(fd, buf, len);
        } else {
            broadcast(fd, buf, len);
        }
    }

    free(buf);
}

/* -------------------------------------------------------------------------
 * Create listening socket (identical to server.c)
 * ---------------------------------------------------------------------- */
static int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--echo") == 0) {
            mode_echo = 1;
        } else {
            fprintf(stderr, "Usage: %s [--port PORT] [--echo]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* epoll_create1(0) is the modern form of epoll_create(size).
     * The size argument is ignored by the kernel but must be > 0 in
     * older interfaces; epoll_create1 drops it entirely. */
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); return 1; }

    listen_fd = create_listen_socket(port);
    if (listen_fd < 0) return 1;

    epoll_add(epoll_fd, listen_fd);

    printf("[epoll] NetForge (epoll) listening on port %d\n", port);
    printf("[epoll] single thread serving all connections via event loop\n");

    struct epoll_event events[MAX_EVENTS];

    /*
     * THE EPOLL EVENT LOOP
     * --------------------
     * epoll_wait() blocks until at least one fd in the watch set becomes
     * ready, then fills `events` with descriptors of ready fds and returns
     * the count. We dispatch each one ourselves, then loop back to wait.
     *
     * Contrast with the pthreads server: there, the kernel schedules a
     * thread for each client; here, WE are the scheduler.
     */
    while (!shutdown_flag) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR) continue;
            if (shutdown_flag) break;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                accept_client(epoll_fd, listen_fd);
            } else {
                handle_readable(epoll_fd, fd);
            }
        }
    }

    /* Cleanup */
    printf("\n[epoll] shutting down...\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (conns[i]) {
            close(conns[i]->fd);
            free(conns[i]);
            conns[i] = NULL;
        }
    }
    if (listen_fd != -1) close(listen_fd);
    close(epoll_fd);

    printf("[epoll] done. goodbye.\n");
    return 0;
}

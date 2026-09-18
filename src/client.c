/*
 * client.c — NetForge interactive CLI client
 *
 * Reads lines from stdin, sends them to the server using our custom
 * length-prefixed protocol, and prints messages received from the server.
 * Runs a receiver thread so it can receive broadcasts while the user types.
 *
 * BUILD:  make client
 * RUN:    ./build/client [--host HOST] [--port PORT]
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9090
#define BUF_SIZE     (MSG_MAX_PAYLOAD + 1)

static volatile int done = 0;

static void client_sig_handler(int sig)
{
    (void)sig;
    done = 1;
}

/* ---------------------------------------------------------------------------
 * Receiver thread — runs in the background, printing incoming broadcasts.
 * The main thread handles stdin → send; this thread handles recv → stdout.
 * --------------------------------------------------------------------------*/
static void *receiver_thread(void *arg)
{
    int fd = *(int *)arg;
    char *buf = malloc(BUF_SIZE);
    if (!buf) return NULL;

    while (!done) {
        uint32_t len = 0;
        int rc = recv_message(fd, buf, BUF_SIZE - 1, &len);
        if (rc != 0) {
            if (!done)
                printf("\n[client] connection closed by server.\n");
            done = 1;
            break;
        }
        buf[len] = '\0';
        printf("\r[recv] %s\n> ", buf);
        fflush(stdout);
    }

    free(buf);
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *host = DEFAULT_HOST;
    int         port = DEFAULT_PORT;
    int         recv_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--listen") == 0)
            recv_only = 1;
        else {
            fprintf(stderr, "Usage: %s [--host HOST] [--port PORT] [--listen]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGINT, client_sig_handler);
    signal(SIGTERM, client_sig_handler);

    /* --- Create TCP socket --------------------------------------------- */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    /* --- Resolve host and connect --------------------------------------- */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Not a dotted-decimal IP — try DNS */
        struct hostent *he = gethostbyname(host);
        if (!he) { fprintf(stderr, "Cannot resolve %s\n", host); return 1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    printf("[client] connected to %s:%d\n", host, port);
    if (recv_only) {
        printf("Running in receive-only mode. Press Ctrl+C to quit.\n\n");
    } else {
        printf("Type a message and press Enter. Ctrl+D to quit.\n\n");
    }

    /* --- Start receiver thread ----------------------------------------- */
    pthread_t rt;
    pthread_create(&rt, NULL, receiver_thread, &fd);
    pthread_detach(rt);

    if (recv_only) {
        while (!done) {
            pause();
        }
    } else {
        /* --- Send loop: read stdin, send to server -------------------------- */
        char line[4096];
        while (!done) {
            printf("> ");
            fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {
            if (!isatty(STDIN_FILENO)) {
                struct timespec ts = {0, 200 * 1000 * 1000L}; /* 200 ms to drain replies */
                nanosleep(&ts, NULL);
            }
            done = 1;
            break;
        }

        /* Strip trailing newline */
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        if (n == 0) continue;

        if (send_message(fd, line, (uint32_t)n) < 0) {
            perror("send_message");
            done = 1;
            break;
        }
    }
    }

    done = 1;
    shutdown(fd, SHUT_RDWR);
    close(fd);
    printf("[client] disconnected.\n");
    return 0;
}

/*
 * udp_server.c — NetForge UDP echo server (Phase 10)
 *
 * UDP vs TCP — Key differences this file demonstrates
 * ----------------------------------------------------
 * TCP (server.c):
 *   socket(AF_INET, SOCK_STREAM, 0)
 *   bind → listen → accept → read/write → close
 *   Connection-oriented: the kernel maintains state for each peer.
 *   A byte stream: one write() ≠ one read() on the other side.
 *
 * UDP (this file):
 *   socket(AF_INET, SOCK_DGRAM, 0)
 *   bind → recvfrom/sendto (forever)
 *   Connectionless: no listen(), no accept(). One socket serves all peers.
 *   Message-oriented: one sendto() = one recvfrom() (if it arrives at all).
 *   No delivery guarantee. No ordering guarantee. No flow control.
 *
 * Why the Phase 4 protocol is irrelevant here
 * --------------------------------------------
 * Our 4-byte length prefix exists because TCP is a byte stream and we need
 * to know where one message ends and the next begins. UDP already preserves
 * message boundaries per datagram — recvfrom() returns exactly the bytes
 * from one sendto() call. No framing needed (though you might still want
 * application-level sequence numbers for out-of-order detection).
 *
 * BUILD:  make udp_server
 * RUN:    ./build/udp_server [--port PORT]
 * TEST:   echo "hello" | nc -u localhost 9091
 *         # nc -u for UDP mode
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT  9091
#define BUF_SIZE      65535   /* max UDP datagram payload */

static volatile sig_atomic_t stop = 0;

static void sigint_handler(int sig) { (void)sig; stop = 1; }

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    signal(SIGINT, sigint_handler);

    /* --- SOCK_DGRAM = UDP ----------------------------------------------- */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    /* bind() works the same as for TCP — associate this fd with a port. */
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(fd); return 1;
    }

    printf("[udp] NetForge UDP echo listening on port %d\n", port);
    printf("[udp] test with: echo \"hello\" | nc -u localhost %d\n", port);

    char *buf = malloc(BUF_SIZE);
    if (!buf) { close(fd); return 1; }

    while (!stop) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof peer_addr;

        /*
         * recvfrom() — like read(), but also fills in the sender's address.
         * We need that address to know where to send the echo back.
         * There is no "connection" here — every datagram can come from a
         * different peer.
         */
        ssize_t n = recvfrom(fd, buf, BUF_SIZE - 1, 0,
                             (struct sockaddr *)&peer_addr, &peer_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }

        buf[n] = '\0';
        char peer[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer_addr.sin_addr, peer, sizeof peer);
        printf("[udp] %s:%u → %zd bytes: %s\n",
               peer, ntohs(peer_addr.sin_port), n, buf);

        /*
         * sendto() — like write(), but we specify the destination address
         * because there is no persistent connection to send "back on".
         */
        ssize_t sent = sendto(fd, buf, (size_t)n, 0,
                              (struct sockaddr *)&peer_addr, peer_len);
        if (sent < 0)
            perror("sendto");
    }

    free(buf);
    close(fd);
    printf("[udp] done.\n");
    return 0;
}

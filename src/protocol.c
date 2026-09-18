/*
 * protocol.c — NetForge message framing implementation (Phase 4)
 *
 * Key insight: a single write() on the sender can produce multiple read()
 * calls on the receiver, or a single read() can return only part of the data.
 * read_n_bytes() is the building block that hides this by looping until it
 * has accumulated exactly the number of bytes requested.
 */

#include "protocol.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>   /* htonl(), ntohl() */

/* ---------------------------------------------------------------------------
 * Internal helper: read exactly n bytes from fd into buf.
 *
 * Loops over read() until n bytes are accumulated, handling:
 *   - Short reads (read() returned fewer bytes than requested — perfectly
 *     normal on a busy socket; we just ask again)
 *   - EINTR  (signal interrupted the syscall; resume transparently)
 *
 * Returns  0 on success (exactly n bytes in buf).
 * Returns  1 on clean EOF before n bytes arrived.
 * Returns -1 on error (errno set).
 * --------------------------------------------------------------------------*/
static int read_n_bytes(int fd, char *buf, uint32_t n)
{
    uint32_t total = 0;

    while (total < n) {
        ssize_t r = read(fd, buf + total, n - total);

        if (r == 0) {
            /* Peer closed connection cleanly */
            return 1;
        }
        if (r < 0) {
            if (errno == EINTR) continue;   /* signal — try again */
            return -1;                      /* real error */
        }

        total += (uint32_t)r;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * send_message — write one length-prefixed frame to fd.
 *
 * Wire format:
 *   [uint32_t len in network byte order][len bytes of payload]
 *
 * We do two write() calls (header then payload). A production implementation
 * might use writev() to combine them into a single syscall, but two writes
 * keeps the code readable for learning purposes.
 * --------------------------------------------------------------------------*/
int send_message(int fd, const char *buf, uint32_t len)
{
    /* Convert length to network (big-endian) byte order */
    uint32_t net_len = htonl(len);

    /* Write the 4-byte header */
    ssize_t w = write(fd, &net_len, MSG_HEADER_SIZE);
    if (w != MSG_HEADER_SIZE) return -1;

    /* Write the payload */
    uint32_t sent = 0;
    while (sent < len) {
        w = write(fd, buf + sent, len - sent);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (uint32_t)w;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * recv_message — read one length-prefixed frame from fd.
 *
 * Returns  0 on success (*out_len holds the payload byte count).
 * Returns  1 on clean EOF.
 * Returns  2 if the declared payload exceeds max_len (message skipped).
 * Returns -1 on I/O error.
 * --------------------------------------------------------------------------*/
int recv_message(int fd, char *buf, uint32_t max_len, uint32_t *out_len)
{
    uint32_t net_len;

    /* Step 1: read the fixed-size 4-byte header */
    int rc = read_n_bytes(fd, (char *)&net_len, MSG_HEADER_SIZE);
    if (rc != 0) return rc;   /* EOF (1) or error (-1) */

    /* Step 2: convert from network byte order to host byte order */
    uint32_t payload_len = ntohl(net_len);

    /* Step 3: sanity check — reject absurdly large messages */
    if (payload_len > MSG_MAX_PAYLOAD || payload_len > max_len) {
        return 2;   /* caller should close this connection */
    }

    /* Step 4: read exactly payload_len bytes of payload */
    rc = read_n_bytes(fd, buf, payload_len);
    if (rc != 0) return rc;

    *out_len = payload_len;
    return 0;
}

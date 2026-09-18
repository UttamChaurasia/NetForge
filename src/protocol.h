/*
 * protocol.h — NetForge custom length-prefixed message protocol
 *
 * DESIGN (Phase 4)
 * ----------------
 * TCP is a byte stream, NOT a message stream. One write() on the sender side
 * does NOT guarantee one read() on the receiver side will get all the bytes
 * (this is called a "short read"). This header defines a tiny framing layer:
 *
 *   [ 4 bytes: uint32_t message length, big-endian (network byte order) ]
 *   [ N bytes: payload                                                   ]
 *
 * Every send_message() call produces exactly one such frame. Every
 * recv_message() call consumes exactly one frame, looping internally until
 * all bytes have arrived (read_n_bytes).
 *
 * The 4-byte header is written with htonl() and read with ntohl() so the
 * protocol is portable across architectures with different endianness.
 */

#ifndef NETFORGE_PROTOCOL_H
#define NETFORGE_PROTOCOL_H

#include <stdint.h>

/* Maximum payload size we accept. Protects against malicious/buggy peers
 * sending a huge length field that would cause us to allocate gigabytes. */
#define MSG_MAX_PAYLOAD (1024 * 1024)   /* 1 MiB */
#define MSG_HEADER_SIZE 4               /* sizeof(uint32_t) */

/*
 * send_message — frame and send a message to fd.
 *
 * Writes the 4-byte big-endian length header followed by `len` bytes of `buf`.
 * Returns 0 on success, -1 on error (sets errno).
 */
int send_message(int fd, const char *buf, uint32_t len);

/*
 * recv_message — receive one framed message from fd.
 *
 * Reads the 4-byte header to discover the payload length, then reads exactly
 * that many bytes into `buf`. Writes the actual payload length into *out_len.
 *
 * Returns  0 on success.
 * Returns -1 on I/O error (sets errno).
 * Returns  1 on clean EOF / peer closed connection.
 * Returns  2 if payload length exceeds max_len (message dropped).
 */
int recv_message(int fd, char *buf, uint32_t max_len, uint32_t *out_len);

#endif /* NETFORGE_PROTOCOL_H */

# NetForge

A multithreaded TCP/UDP server built in C — a phased systems-programming project covering socket programming, concurrency, protocol design, I/O multiplexing, and packet analysis.

## What's inside

| File | Phase | What it covers |
|------|-------|----------------|
| `src/server.c` | 1–7 | TCP server: single-client echo → fork → pthreads + mutex → custom protocol → broadcast chat → signal handling → daemonize |
| `src/client.c` | 1–7 | Interactive CLI client with background receiver thread |
| `src/protocol.c/h` | 4 | Length-prefixed message framing (the `read_n_bytes` loop, `htonl`/`ntohl`) |
| `src/daemon.c/h` | 7 | Double-fork daemonization, PID file |
| `src/server_epoll.c` | 9 | Single-threaded epoll event loop — same broadcast chat, zero pthreads |
| `src/udp_server.c` | 10 | UDP echo: `SOCK_DGRAM`, `recvfrom`/`sendto` |
| `src/pcap_tool.c` | 11 | libpcap: walk Ethernet→IP→TCP headers, parse NetForge frame from raw bytes |

## Environment

- **OS**: Linux (WSL2, native, or VM)
- **Compiler**: gcc (tested with gcc 15 on Ubuntu)
- **Required packages**:
  ```bash
  # Ubuntu/Debian
  sudo apt-get install -y gcc make gdb libpcap-dev

  # Alpine
  apk add gcc make gdb musl-dev libpcap-dev
  ```

## Build

```bash
make            # build everything (debug)
make release    # build everything optimised
make server     # pthreads server only
make client     # client only
make epoll_server
make udp_server
make pcap_tool  # requires libpcap-dev
make clean
```

All binaries land in `build/`.

## Running — Phase by Phase

### Phase 1–6: pthreads broadcast chat server

Terminal 1 — server:
```bash
./build/server --port 9090
```

Terminal 2, 3, … — clients:
```bash
./build/client --port 9090
```

Type in any client. The message appears in all others. `Ctrl+C` the server for clean shutdown.

### Phase 7: daemon mode
```bash
./build/server --daemon --port 9090
# Server detaches. PID written to ./netforge.pid
kill $(cat netforge.pid)
```

### Phase 9: epoll server (single thread)
```bash
./build/epoll_server --port 9090
# Confirm single thread: ps -T -p $(pgrep epoll_server)
```
Same clients work unchanged.

### Phase 10: UDP echo
```bash
./build/udp_server --port 9091
# Test:
echo "hello" | nc -u localhost 9091
```

### Phase 11: libpcap live capture
```bash
# Terminal 1: run the server
./build/server --port 9090

# Terminal 2: capture (needs root for raw socket)
sudo ./build/pcap_tool --port 9090 --iface lo

# Terminals 3+: connect clients and chat
./build/client --port 9090
```
The pcap tool prints `NetForge frame: declared payload = N bytes` and a content preview for every message sent.

### Phase 8: Wireshark / tcpdump inspection

Capture your protocol on the wire:
```bash
# Capture to file (no Wireshark needed)
sudo tcpdump -i lo -w capture.pcap port 9090

# Run server + clients in other terminals, then stop tcpdump with Ctrl+C

# Inspect with tcpdump (text mode)
tcpdump -r capture.pcap -X port 9090 | head -60

# Or open capture.pcap in Wireshark on Windows
# Look for the 4-byte big-endian length in the hex pane
# (e.g. "hello" = 5 bytes → header = 00 00 00 05, payload = 68 65 6c 6c 6f)
```

## Key concepts by phase

| Phase | Concept to understand |
|-------|-----------------------|
| 1 | `accept()` returns a NEW fd; the listening fd stays open |
| 1 | `htons()`/`htonl()` — why byte order matters across networks |
| 1 | `SO_REUSEADDR` — prevents `Address already in use` on restart |
| 2 | `fork()` duplicates the fd table; both parent and child must close what they don't own |
| 2 | `SIGCHLD` + `waitpid(WNOHANG)` — reaping zombie processes |
| 3 | Shared address space between threads — faster than fork, more dangerous |
| 3 | Race condition on `client_list[]` → deliberate bug → `pthread_mutex_t` fix |
| 3 | `pthread_detach()` — thread zombie prevention |
| 4 | TCP is a byte stream: one `write()` ≠ one `read()` |
| 4 | `read_n_bytes()` — the loop that actually solves short reads |
| 5 | Write under mutex; handle `write()` failure mid-broadcast gracefully |
| 6 | `volatile sig_atomic_t` — why signal handlers need special variable types |
| 6 | `SIGPIPE` → `SIG_IGN` — don't let a disconnected client kill the server |
| 7 | Double-fork pattern — why two `fork()` calls are needed to detach fully |
| 9 | Level-triggered vs edge-triggered epoll |
| 9 | One thread, N connections: you become the scheduler |
| 10 | `SOCK_DGRAM`: connectionless, message-oriented, no framing needed |
| 11 | `ihl * 4` + `doff * 4` — navigating variable-length IP/TCP headers |
| 11 | BPF filters run in the kernel — far more efficient than userspace filtering |

## Resume bullet (fill in what you've actually built)

> Built **NetForge**, a multithreaded TCP/UDP server in C, supporting concurrent client connections via both pthreads and an epoll-based event loop, with a custom length-prefixed message protocol validated via libpcap packet capture; implemented graceful shutdown via signal handling and optional process daemonization.

Trim to match exactly what you finished. A bullet for Phases 1–6 only is already interview-proof.

## Phase 12 (deferred)

Once a separate kernel character device driver project exists, point the server's message storage at `/dev/mychardev` instead of the in-memory buffer. See the handbook for details.

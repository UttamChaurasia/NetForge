# NetForge

A Linux-based networking and systems programming project written in C.

NetForge is a hands-on implementation of core networking and operating-system concepts, progressing from a basic TCP client/server to concurrent servers, custom application-layer framing, event-driven I/O, UDP communication, daemonization, and packet capture.

The project is designed to explore how network services work internally rather than relying entirely on high-level networking frameworks.

---

## Features

- TCP client-server communication
- TCP byte-stream handling
- Custom length-prefixed application protocol
- Concurrent client handling using `fork()`
- Multi-threaded client handling using POSIX threads
- Thread-safe client registry using mutexes
- Server-side message broadcasting
- Graceful signal-based shutdown
- `SIGCHLD` handling for child processes
- `SIGPIPE` protection
- Event-driven TCP server using `epoll`
- UDP server implementation
- Linux daemonization
- PID file management
- Packet capture and inspection using `libpcap`
- Automated system-level testing
- Make-based build system

---

## Architecture

```text
                         NetForge
                            |
          +-----------------+------------------+
          |                 |                  |
       TCP Stack         UDP Stack        Packet Capture
          |                 |                  |
    +-----+------+          |             libpcap
    |            |          |
 TCP Server   TCP Client   UDP Server
    |
    +----------------------+
    |                      |
 fork() / pthreads       epoll
    |                      |
 concurrent            event-driven
 clients               I/O
    |
    +----------------------+
    |
 Custom Protocol
    |
 [4-byte length][payload]
Project Structure
NetForge/
├── .gitignore
├── Makefile
├── README.md
├── LICENSE
├── test_all.sh
│
├── build/
│   └── .gitkeep
│
└── src/
    ├── client.c
    ├── server.c
    ├── server_epoll.c
    ├── udp_server.c
    ├── protocol.c
    ├── protocol.h
    ├── daemon.c
    ├── daemon.h
    └── pcap_tool.c
Components
1. TCP Client and Server

The basic TCP implementation demonstrates:

socket()
bind()
listen()
accept()
connect()
send()
recv()
close()

The server accepts TCP clients and handles communication with them.

2. Concurrent TCP Server

NetForge explores multiple concurrency models.

Process-based concurrency
             Server
                |
             accept()
                |
        +-------+-------+
        |               |
      Client 1        Client 2
        |               |
      fork()           fork()

Each client can be handled by a separate process.

Thread-based concurrency
             Server
                |
             accept()
                |
        +-------+-------+
        |       |       |
     Thread1 Thread2 Thread3
        |       |       |
      Client  Client  Client

A mutex-protected client registry is used for shared state.

3. Custom Application Protocol

TCP provides a byte stream rather than message boundaries.

NetForge therefore implements a length-prefixed protocol:

+----------------------+----------------------+
| 4-byte message size  |      payload         |
+----------------------+----------------------+
       uint32_t
       network byte order

The protocol uses:

htonl()
ntohl()

to maintain network byte order.

This allows the receiver to determine exactly how many bytes belong to one application-level message.

4. Event-Driven Server

server_epoll.c implements an event-driven TCP server using Linux epoll.

             epoll instance
                   |
             epoll_wait()
                   |
        +----------+----------+
        |          |          |
      Client 1   Client 2   Client 3
       ready      ready      ready

This demonstrates readiness-based I/O without creating a dedicated thread or process for every connection.

5. UDP Server

udp_server.c implements connectionless UDP communication using:

recvfrom()
sendto()

Unlike TCP, UDP preserves datagram boundaries.

6. Daemon Mode

NetForge contains Linux daemonization functionality using the traditional double-fork approach.

The daemon component demonstrates:

fork()
parent termination
setsid()
changing the working directory
redirecting standard file descriptors
PID file creation
7. Packet Capture Tool

pcap_tool.c uses libpcap to inspect network traffic.

The tool demonstrates:

opening a live network interface
packet capture
BPF filtering
Ethernet frame parsing
IPv4 parsing
TCP parsing
NetForge application-protocol inspection
Build
Requirements

Linux / WSL Ubuntu

Install the required packages:

sudo apt update
sudo apt install build-essential libpcap-dev

Build the project:

make

Compiled binaries are placed in:

build/

Build artifacts are intentionally ignored by Git.

Running
TCP Server

Example:

./build/server --port 9090
TCP Client

Connect the client to the running server using the command-line options supported by the client.

Multiple clients can be connected simultaneously to test concurrent communication and broadcasting.

Epoll Server
./build/epoll_server
UDP Server
./build/udp_server
Packet Capture Tool

The packet capture tool requires libpcap and may require appropriate Linux privileges depending on the network interface and capture configuration.

Testing

Run the complete project test suite:

./test_all.sh

The test script exercises the implemented networking components and verifies expected system behavior.

Networking Concepts Demonstrated

NetForge focuses on practical implementation of:

TCP vs UDP
Client-server architecture
TCP byte streams
Message framing
Partial reads and writes
Network byte order
Socket lifecycle
Blocking I/O
Concurrent processes
POSIX threads
Mutex synchronization
Signal handling
Zombie-process prevention
SIGPIPE
Linux epoll
Event-driven I/O
Daemon processes
Packet capture
Ethernet/IP/TCP packet parsing
Learning Goals

The primary goal of NetForge is to understand the systems underneath network applications.

The project is intentionally implemented close to the Linux socket and process/thread APIs so that concepts such as:

socket
   ↓
bind
   ↓
listen
   ↓
accept
   ↓
read/write
   ↓
concurrency
   ↓
protocol framing
   ↓
event-driven I/O

can be understood through actual implementation.

Development Approach

NetForge is developed incrementally using Git.

Major components are introduced through separate commits so that the evolution of the networking stack can be studied through the Git history.

Author

Uttam Chaurasia
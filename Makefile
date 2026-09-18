# ==============================================================================
# NetForge — Multithreaded TCP/UDP Server in C
# Makefile
# ==============================================================================

CC      := gcc
SRCDIR  := src
BUILDDIR:= build

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
# -Wall -Wextra  : maximum warnings (treat your compiler as a free code review)
# -pthread       : enables POSIX thread API and links libpthread
# -g             : debug symbols (default target)
# -O2            : optimise (release target)
# ---------------------------------------------------------------------------
CFLAGS_COMMON := -Wall -Wextra -std=c11 -pthread
CFLAGS_DEBUG  := $(CFLAGS_COMMON) -g -DDEBUG
CFLAGS_RELEASE:= $(CFLAGS_COMMON) -O2 -DNDEBUG
CFLAGS        := $(CFLAGS_DEBUG)   # default: debug

LDFLAGS       := -pthread
PCAP_LDFLAGS  := -lpcap

# ---------------------------------------------------------------------------
# Sources & Targets
# ---------------------------------------------------------------------------
SERVER_SRCS    := $(SRCDIR)/server.c   $(SRCDIR)/protocol.c $(SRCDIR)/daemon.c
CLIENT_SRCS    := $(SRCDIR)/client.c   $(SRCDIR)/protocol.c
EPOLL_SRCS     := $(SRCDIR)/server_epoll.c $(SRCDIR)/protocol.c
UDP_SRCS       := $(SRCDIR)/udp_server.c
PCAP_SRCS      := $(SRCDIR)/pcap_tool.c

SERVER_BIN     := $(BUILDDIR)/server
CLIENT_BIN     := $(BUILDDIR)/client
EPOLL_BIN      := $(BUILDDIR)/epoll_server
UDP_BIN        := $(BUILDDIR)/udp_server
PCAP_BIN       := $(BUILDDIR)/pcap_tool

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all server client epoll_server udp_server pcap_tool debug release clean

all: server client epoll_server udp_server pcap_tool

server: $(SERVER_BIN)
client: $(CLIENT_BIN)
epoll_server: $(EPOLL_BIN)
udp_server: $(UDP_BIN)
pcap_tool: $(PCAP_BIN)

$(SERVER_BIN): $(SERVER_SRCS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[netforge] built: $@"

$(CLIENT_BIN): $(CLIENT_SRCS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[netforge] built: $@"

$(EPOLL_BIN): $(EPOLL_SRCS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[netforge] built: $@"

$(UDP_BIN): $(UDP_SRCS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[netforge] built: $@"

$(PCAP_BIN): $(PCAP_SRCS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(PCAP_LDFLAGS)
	@echo "[netforge] built: $@"

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Convenience aliases
debug:
	$(MAKE) CFLAGS="$(CFLAGS_DEBUG)" all

release:
	$(MAKE) CFLAGS="$(CFLAGS_RELEASE)" all

clean:
	rm -rf $(BUILDDIR)
	@echo "[netforge] cleaned."

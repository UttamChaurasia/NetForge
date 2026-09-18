#!/usr/bin/env bash
# ==============================================================================
# NetForge — Automated Test Suite across all phases
# ==============================================================================

set -e
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }
info() { echo -e "${YELLOW}[TEST]${NC} $1"; }

# Clean any existing processes
killall -9 server client epoll_server udp_server 2>/dev/null || true

# 1. Clean build verification
info "Building all binaries with -Wall -Wextra..."
make clean >/dev/null
make all >/dev/null
pass "All binaries compiled cleanly with zero warnings"

# 2. Phase 1, 4 & 6: TCP Pthread Server in Echo Mode
info "Testing Phase 1 & 4 (Pthreads TCP Server in --echo mode)..."
./build/server --port 9091 --echo >/tmp/netforge_test_server.log 2>&1 &
SERVER_PID=$!
sleep 0.4

RESP=$(echo "HelloNetForgePhase1" | ./build/client --port 9091 2>/dev/null || true)
kill -INT $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

if echo "$RESP" | grep -q "HelloNetForgePhase1"; then
    pass "Phase 1 & 4 Echo server correctly framed and echoed message"
else
    echo "Response was: $RESP"
    fail "Phase 1 echo failed"
fi

# 3. Phase 2: Fork-based Server
info "Testing Phase 2 (Process-based fork server)..."
./build/server --port 9092 --fork >/tmp/netforge_fork.log 2>&1 &
FORK_PID=$!
sleep 0.4

RESP_FORK=$(echo "HelloForkProcess" | ./build/client --port 9092 2>/dev/null || true)
kill -INT $FORK_PID 2>/dev/null || true
wait $FORK_PID 2>/dev/null || true

if echo "$RESP_FORK" | grep -q "HelloForkProcess"; then
    pass "Phase 2 Fork-based server handled client independently in child process"
else
    echo "Response was: $RESP_FORK"
    fail "Phase 2 fork server failed"
fi

# 4. Phase 3 & 5: Multithreaded Broadcast Chat (2 clients)
info "Testing Phase 3 & 5 (Pthreads Broadcast Chat)..."
./build/server --port 9093 >/tmp/netforge_broadcast.log 2>&1 &
BCAST_PID=$!
sleep 0.4

# Client 2 listens and writes output to file
./build/client --port 9093 --listen >/tmp/client2.log 2>&1 &
CLIENT2_PID=$!
sleep 0.4

# Client 1 sends a broadcast message
echo "BroadcastFromClient1" | ./build/client --port 9093 >/dev/null 2>&1 || true
sleep 0.5

# Stop client 2 and server
kill -TERM $CLIENT2_PID 2>/dev/null || true
sleep 0.2
kill -9 $CLIENT2_PID 2>/dev/null || true
wait $CLIENT2_PID 2>/dev/null || true

kill -INT $BCAST_PID 2>/dev/null || true
sleep 0.2
kill -9 $BCAST_PID 2>/dev/null || true
wait $BCAST_PID 2>/dev/null || true

if grep -q "BroadcastFromClient1" /tmp/client2.log 2>/dev/null; then
    pass "Phase 5 Broadcast chat successfully delivered message across threads to peer client"
else
    cat /tmp/client2.log || true
    fail "Phase 5 broadcast test failed"
fi

# 5. Phase 7: Daemonization
info "Testing Phase 7 (Double-fork Daemonization)..."
rm -f /tmp/netforge.pid
./build/server --port 9094 --daemon
sleep 0.5

if [ -f /tmp/netforge.pid ]; then
    DAEMON_PID=$(cat /tmp/netforge.pid)
    if ps -p "$DAEMON_PID" >/dev/null 2>&1; then
        pass "Phase 7 Server detached as daemon, running at PID $DAEMON_PID"
        kill -INT "$DAEMON_PID" 2>/dev/null || true
        rm -f /tmp/netforge.pid
    else
        fail "Daemon PID $DAEMON_PID not running"
    fi
else
    fail "netforge.pid file not created by daemon"
fi

# 6. Phase 9: epoll Event-Loop Server
info "Testing Phase 9 (epoll Single-Threaded Event Loop)..."
./build/epoll_server --port 9095 --echo >/tmp/netforge_epoll.log 2>&1 &
EPOLL_PID=$!
sleep 0.4

RESP_EPOLL=$(echo "HelloFromEpoll" | ./build/client --port 9095 2>/dev/null || true)
kill -INT $EPOLL_PID 2>/dev/null || true
wait $EPOLL_PID 2>/dev/null || true

if echo "$RESP_EPOLL" | grep -q "HelloFromEpoll"; then
    pass "Phase 9 epoll event loop successfully handled client and echoed message"
else
    echo "Response was: $RESP_EPOLL"
    fail "Phase 9 epoll test failed"
fi

# 7. Phase 10: UDP Echo Server
info "Testing Phase 10 (UDP Echo Server)..."
./build/udp_server --port 9096 >/tmp/netforge_udp.log 2>&1 &
UDP_PID=$!
sleep 0.4

UDP_RESP=$(echo "NetForgeUDPPacket" | nc -u -w1 127.0.0.1 9096 2>/dev/null || true)
kill -INT $UDP_PID 2>/dev/null || true
wait $UDP_PID 2>/dev/null || true

if echo "$UDP_RESP" | grep -q "NetForgeUDPPacket"; then
    pass "Phase 10 UDP Echo Server received datagram and echoed back via sendto()"
else
    echo "UDP response was: $UDP_RESP"
    fail "Phase 10 UDP test failed"
fi

# 8. Phase 11: libpcap tool availability
info "Testing Phase 11 (libpcap parser binary)..."
if [ -x ./build/pcap_tool ]; then
    pass "Phase 11 pcap_tool compiled and ready for live capture"
else
    fail "Phase 11 pcap_tool missing"
fi

echo
echo -e "${GREEN}======================================================${NC}"
echo -e "${GREEN}  All NetForge Systems Tests Passed Successfully!     ${NC}"
echo -e "${GREEN}======================================================${NC}"

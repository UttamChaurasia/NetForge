/*
 * pcap_tool.c — NetForge libpcap protocol parser (Phase 11)
 *
 * _DEFAULT_SOURCE enables BSD-era types (u_char, u_int, u_short) that
 * libpcap headers require. Must be defined before any system includes.
 */
#define _DEFAULT_SOURCE
/*
 * This tool captures live traffic on the loopback interface and parses the
 * NetForge 4-byte length-prefixed header out of raw TCP payload bytes.
 *
 * WHAT THIS TEACHES
 * -----------------
 * Every packet on the wire is structured as a stack of headers:
 *
 *   [ Ethernet header (14 bytes) ]
 *   [ IP header      (≥20 bytes) ]
 *   [ TCP header     (≥20 bytes) ]
 *   [ Application payload        ]  ← our NetForge length prefix lives here
 *
 * We walk this stack by hand using pointer arithmetic. This is why "protocol
 * stack" stops being an abstract phrase after this phase — you will have
 * literally navigated it with a pointer.
 *
 * IMPORTANT CAVEATS
 * -----------------
 * - This is a live packet observer, not a stream reassembler. TCP can split
 *   one application message across multiple packets. For simplicity, we
 *   parse only packets whose payload begins with a valid NetForge header
 *   (i.e. we might miss messages split across packets). A full reassembler
 *   requires tracking per-stream state — that's a separate project.
 * - Requires root (or CAP_NET_RAW) to open a live pcap handle.
 *
 * BUILD:  make pcap_tool          (links -lpcap)
 * RUN:    sudo ./build/pcap_tool [--port PORT] [--iface IFACE]
 *         # then run server and clients in other terminals
 *
 * REQUIRES: libpcap-dev
 *   Ubuntu/Debian: sudo apt-get install libpcap-dev
 *   Alpine:        apk add libpcap-dev
 */


#include <arpa/inet.h>
#include <netinet/if_ether.h>   /* struct ethhdr */
#include <netinet/ip.h>         /* struct iphdr  */
#include <netinet/tcp.h>        /* struct tcphdr */
#include <pcap.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"           /* MSG_HEADER_SIZE */

#define DEFAULT_PORT   9090
#define DEFAULT_IFACE  "lo"     /* loopback — where local server traffic flows */
#define SNAP_LEN       65535    /* capture up to this many bytes per packet */
#define PROMISC        0        /* no promiscuous mode needed for loopback */
#define TIMEOUT_MS     1000     /* pcap read timeout in milliseconds */

static pcap_t *handle = NULL;
static volatile sig_atomic_t stop = 0;

static void sigint_handler(int sig) { (void)sig; stop = 1; if (handle) pcap_breakloop(handle); }

/* -------------------------------------------------------------------------
 * packet_handler — called by pcap for every captured packet
 *
 * Parameters (mandated by pcap's callback signature):
 *   user    — user-supplied pointer (we pass the server port)
 *   header  — pcap metadata: timestamp, captured length, wire length
 *   packet  — raw packet bytes starting at the link-layer header
 * ---------------------------------------------------------------------- */
static void packet_handler(u_char *user,
                            const struct pcap_pkthdr *header,
                            const u_char *packet)
{
    int server_port = *(int *)user;
    (void)header;   /* we don't use the timestamp in this tool */

    /* ------------------------------------------------------------------
     * Step 1: Ethernet header
     * The loopback interface uses a Linux "cooked" capture (DLT_LINUX_SLL)
     * or real Ethernet framing depending on the pcap link type. We handle
     * the common Ethernet case; the BPF filter already narrowed us to TCP.
     * ------------------------------------------------------------------ */
    const struct ethhdr *eth = (const struct ethhdr *)packet;
    uint16_t eth_type = ntohs(eth->h_proto);

    /* Only handle IPv4 for now */
    if (eth_type != ETH_P_IP) return;

    /* ------------------------------------------------------------------
     * Step 2: IP header
     * ihl (Internet Header Length) is in 32-bit words. Multiply by 4 to
     * get bytes. The IP header can be 20–60 bytes long due to options.
     * ------------------------------------------------------------------ */
    const struct iphdr *ip = (const struct iphdr *)(packet + sizeof(struct ethhdr));
    size_t ip_hdr_len = (size_t)(ip->ihl) * 4;

    if (ip->protocol != IPPROTO_TCP) return;

    /* ------------------------------------------------------------------
     * Step 3: TCP header
     * doff (Data OFfset) is in 32-bit words. Multiply by 4 for bytes.
     * The TCP header can be 20–60 bytes due to options (timestamps, SACK…).
     * ------------------------------------------------------------------ */
    const struct tcphdr *tcp = (const struct tcphdr *)
                               ((const u_char *)ip + ip_hdr_len);
    size_t tcp_hdr_len = (size_t)(tcp->doff) * 4;

    /* Filter: only packets to/from our server port */
    uint16_t src_port = ntohs(tcp->source);
    uint16_t dst_port = ntohs(tcp->dest);
    if (src_port != (uint16_t)server_port && dst_port != (uint16_t)server_port)
        return;

    /* ------------------------------------------------------------------
     * Step 4: Application payload — the NetForge frame starts here
     * ------------------------------------------------------------------ */
    const u_char *payload = (const u_char *)tcp + tcp_hdr_len;

    /* Total packet bytes captured by pcap */
    uint32_t captured = header->caplen;

    /* Offset of payload within the captured buffer */
    size_t payload_offset = (size_t)(payload - packet);

    if (captured <= payload_offset) return;   /* no payload bytes */
    uint32_t payload_len = (uint32_t)(captured - payload_offset);

    /* We need at least 4 bytes to read the NetForge length header */
    if (payload_len < MSG_HEADER_SIZE) return;

    /* Read the 4-byte length prefix (big-endian) */
    uint32_t net_len;
    memcpy(&net_len, payload, MSG_HEADER_SIZE);
    uint32_t msg_len = ntohl(net_len);

    /* Sanity: a 0-byte message or implausibly large length is probably not
     * a NetForge frame (could be a TCP control segment with no payload). */
    if (msg_len == 0 || msg_len > MSG_MAX_PAYLOAD) return;

    printf("[pcap] port %u→%u | NetForge frame: declared payload = %u bytes\n",
           src_port, dst_port, msg_len);

    /* Bonus: if the full payload is in this packet, print the content */
    if (payload_len >= MSG_HEADER_SIZE + msg_len) {
        char preview[81];
        uint32_t show = msg_len < 80 ? msg_len : 80;
        memcpy(preview, payload + MSG_HEADER_SIZE, show);
        preview[show] = '\0';
        printf("[pcap]   content preview: \"%s\"%s\n",
               preview, msg_len > 80 ? "..." : "");
    }
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int         port  = DEFAULT_PORT;
    const char *iface = DEFAULT_IFACE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port")  == 0 && i + 1 < argc) port  = atoi(argv[++i]);
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) iface = argv[++i];
    }

    signal(SIGINT, sigint_handler);

    char errbuf[PCAP_ERRBUF_SIZE];

    /* --- Open live capture handle --------------------------------------- */
    handle = pcap_open_live(iface, SNAP_LEN, PROMISC, TIMEOUT_MS, errbuf);
    if (!handle) {
        fprintf(stderr, "[pcap] pcap_open_live(%s): %s\n", iface, errbuf);
        fprintf(stderr, "[pcap] hint: run with sudo, or check iface name\n");
        return 1;
    }

    /* --- Verify link type -----------------------------------------------
     * DLT_EN10MB = standard Ethernet (also used by loopback on Linux).
     * DLT_LINUX_SLL = Linux "cooked" capture mode. Adjust ethhdr parsing
     * if you get link type 113 (Linux SLL). For simplicity we require EN10MB.
     * -------------------------------------------------------------------- */
    int link_type = pcap_datalink(handle);
    if (link_type != DLT_EN10MB) {
        fprintf(stderr, "[pcap] unsupported link type %d on %s\n"
                        "[pcap] try: --iface eth0, or see man pcap-linktype\n",
                link_type, iface);
        pcap_close(handle);
        return 1;
    }

    /* --- Compile + apply BPF filter ------------------------------------
     * The BPF filter runs in the kernel: only packets matching it are
     * handed to our callback. This is far more efficient than receiving
     * all packets and filtering in userspace.
     * ------------------------------------------------------------------- */
    char filter_expr[64];
    snprintf(filter_expr, sizeof filter_expr, "tcp port %d", port);

    struct bpf_program fp;
    if (pcap_compile(handle, &fp, filter_expr, 0, PCAP_NETMASK_UNKNOWN) < 0) {
        fprintf(stderr, "[pcap] pcap_compile: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        return 1;
    }
    if (pcap_setfilter(handle, &fp) < 0) {
        fprintf(stderr, "[pcap] pcap_setfilter: %s\n", pcap_geterr(handle));
        pcap_freecode(&fp);
        pcap_close(handle);
        return 1;
    }
    pcap_freecode(&fp);

    printf("[pcap] capturing on %s, filter: \"%s\"\n", iface, filter_expr);
    printf("[pcap] waiting for NetForge traffic... (Ctrl+C to stop)\n\n");

    /* --- Main capture loop ---------------------------------------------- */
    pcap_loop(handle, -1, packet_handler, (u_char *)&port);

    pcap_close(handle);
    printf("\n[pcap] capture stopped.\n");
    return 0;
}

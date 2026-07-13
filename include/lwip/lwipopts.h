#pragma once
// lwIP options for NetHID on Pico 2 W
// The CYW43 driver already sets many defaults; we only override what we need.

// lwIP is compiled separately from the app, so CMake -D flags do NOT reach it.
// Pulling in config.h here is how the TLS switch below sees ENABLE_HTTPS — which
// is why ENABLE_HTTPS must be set in config.h itself (not only via CMake).
#include "config.h"


// Memory pools — enough for 5 simultaneous connections (4 clients + HTTP)
#define MEMP_NUM_TCP_PCB            6
#define MEMP_NUM_TCP_PCB_LISTEN     3
#define MEM_SIZE                    8000

// Checksum — let hardware do it where available
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1

// Common settings used in most of the pico_w examples
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

// allow override in some examples
#ifndef NO_SYS
#define NO_SYS                      1
#endif
// allow override in some examples
#ifndef LWIP_SOCKET
#define LWIP_SOCKET                 0
#endif
#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC             1
#else
// MEM_LIBC_MALLOC is incompatible with non polling versions
#define MEM_LIBC_MALLOC             0
#endif
#define MEM_ALIGNMENT               4
#ifndef MEM_SIZE
#define MEM_SIZE                    4000
#endif
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETCONN                0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
// #define ETH_PAD_SIZE                2
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif

#define ETHARP_DEBUG                LWIP_DBG_OFF
#define NETIF_DEBUG                 LWIP_DBG_OFF
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define API_LIB_DEBUG               LWIP_DBG_OFF
#define API_MSG_DEBUG               LWIP_DBG_OFF
#define SOCKETS_DEBUG               LWIP_DBG_OFF
#define ICMP_DEBUG                  LWIP_DBG_OFF
#define INET_DEBUG                  LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF
#define IP_REASS_DEBUG              LWIP_DBG_OFF
#define RAW_DEBUG                   LWIP_DBG_OFF
#define MEM_DEBUG                   LWIP_DBG_OFF
#define MEMP_DEBUG                  LWIP_DBG_OFF
#define SYS_DEBUG                   LWIP_DBG_OFF
#define TCP_DEBUG                   LWIP_DBG_OFF
#define TCP_INPUT_DEBUG             LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG            LWIP_DBG_OFF
#define TCP_RTO_DEBUG               LWIP_DBG_OFF
#define TCP_CWND_DEBUG              LWIP_DBG_OFF
#define TCP_WND_DEBUG               LWIP_DBG_OFF
#define TCP_FR_DEBUG                LWIP_DBG_OFF
#define TCP_QLEN_DEBUG              LWIP_DBG_OFF
#define TCP_RST_DEBUG               LWIP_DBG_OFF
#define UDP_DEBUG                   LWIP_DBG_OFF
#define TCPIP_DEBUG                 LWIP_DBG_OFF
#define PPP_DEBUG                   LWIP_DBG_OFF
#define SLIP_DEBUG                  LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_OFF


// ── TLS (altcp_tls + mbedTLS) — only when ENABLE_HTTPS=1 in config.h ──────────
// VERIFY ON HARDWARE: option names below match lwIP in Pico SDK 2.2.0; cross-
// check against pico-examples/pico_w/wifi/tls_* if the build complains.
#if ENABLE_HTTPS
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
// A TLS listener is TWO altcp pcbs (inner altcp_tcp + outer altcp_tls wrapper),
// and EACH accepted connection is also two (accepted inner + TLS wrapper); a plain
// altcp_tcp listener/connection is one each. With TLS now on BOTH the web server
// and the control socket (plus the plain :9000 listener), size generously:
//   web TLS:    2 (listener) + 2*MAX_HTTP_CONN(4)        = 10
//   socket TCP: 1 (listener) + 1*MAX_TCP_CLIENTS(4)      =  5
//   socket TLS: 2 (listener) + 2*MAX_TCP_CLIENTS(4)      = 10
// (Undersizing makes altcp_mbedtls_lower_accept fail its pcb alloc silently and
// reset the connection before mbedTLS ever runs — sized to comfortably exceed 25.)
#define MEMP_NUM_ALTCP_PCB          30
// Every connection (plain or TLS) has an underlying TCP pcb: web + :9000 + :9443.
#undef  MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB            16
#undef  MEMP_NUM_TCP_PCB_LISTEN
#define MEMP_NUM_TCP_PCB_LISTEN     4
// TLS record buffers and mbedTLS allocations need more lwIP heap. Tune per board
// (RP2040 has 264 KB total SRAM; RP2350 has 520 KB).
#undef  MEM_SIZE
#if PICO_RP2040
#define MEM_SIZE                    16000
#else
#define MEM_SIZE                    49152
#endif

// Keep TCP_WND comfortably above the TLS record input buffer
// (MBEDTLS_SSL_IN_CONTENT_LEN, 8192) so a full record fits the receive window.
#undef  TCP_WND
#define TCP_WND                     (12 * TCP_MSS)
#undef  PBUF_POOL_SIZE
#define PBUF_POOL_SIZE              32
#undef  MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG            32
#endif

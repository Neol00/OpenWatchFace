/* lwipopts.h — lwIP 2.1 for the snapdragon-port bare-metal runtime.
 * NO_SYS: the stack is driven from ONE task (wlan_net.c) under one lock;
 * the Arduino-facing classes call in under the same lock. Raw API only. */
#pragma once
#define NO_SYS                      1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_TIMERS                 1
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (96 * 1024)
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_UDP_PCB            6
#define MEMP_NUM_TCP_PCB            6
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            64
#define MEMP_NUM_SYS_TIMEOUT        12
#define PBUF_POOL_SIZE              48
#define PBUF_POOL_BUFSIZE           1600
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)   /* v203: 23 KB; the per-connection ring is 64 KB */
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_STATS                  0
#define LWIP_CHECKSUM_ON_COPY       0
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define ETH_PAD_SIZE                0
#define LWIP_RAND()                 ((u32_t)owf_lwip_rand())
#define LWIP_DEBUG                  0
#define LWIP_NOASSERT               1
/* SNTP app */
#define SNTP_SERVER_DNS             1
#define SNTP_MAX_SERVERS            2
#define SNTP_UPDATE_DELAY           3600000
#define SNTP_STARTUP_DELAY          0
#define SNTP_SET_SYSTEM_TIME(sec)   owf_lwip_set_time((unsigned long)(sec))
#define SNTP_CHECK_RESPONSE         1
#ifdef __cplusplus
extern "C" {
#endif
unsigned long owf_lwip_rand(void);
void owf_lwip_set_time(unsigned long sec);
#ifdef __cplusplus
}
#endif

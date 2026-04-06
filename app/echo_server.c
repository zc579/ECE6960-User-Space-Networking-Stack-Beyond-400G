#include <arpa/inet.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define BURST_SIZE 32

static volatile bool force_quit;

struct app_config {
    uint32_t local_ip_be;
    uint16_t port_id;
    uint16_t queue_id;
};

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        force_quit = true;
    }
}

static void
print_usage(const char *progname)
{
    fprintf(stderr, "Usage: sudo %s <server-ip>\n", progname);
    fprintf(stderr, "Example: sudo %s 10.16.1.1\n", progname);
}

static int
parse_ipv4_arg(const char *arg, uint32_t *ip_be)
{
    struct in_addr addr;

    if (inet_pton(AF_INET, arg, &addr) != 1) {
        return -1;
    }

    *ip_be = addr.s_addr;
    return 0;
}

static int
find_interface_mac_by_ip(uint32_t ip_be, struct rte_ether_addr *mac, char *ifname, size_t ifname_len)
{
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    char target_name[IF_NAMESIZE] = {0};
    int found = -1;

    if (getifaddrs(&ifaddr) != 0) {
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        struct sockaddr_in *addr_in;

        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        addr_in = (struct sockaddr_in *)ifa->ifa_addr;
        if (addr_in->sin_addr.s_addr == ip_be) {
            snprintf(target_name, sizeof(target_name), "%s", ifa->ifa_name);
            break;
        }
    }

    if (target_name[0] == '\0') {
        freeifaddrs(ifaddr);
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        struct sockaddr_ll *addr_ll;

        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_PACKET) {
            continue;
        }
        if (strcmp(ifa->ifa_name, target_name) != 0) {
            continue;
        }

        addr_ll = (struct sockaddr_ll *)ifa->ifa_addr;
        if (addr_ll->sll_halen != RTE_ETHER_ADDR_LEN) {
            continue;
        }

        memcpy(mac->addr_bytes, addr_ll->sll_addr, RTE_ETHER_ADDR_LEN);
        if (ifname != NULL && ifname_len > 0) {
            snprintf(ifname, ifname_len, "%s", target_name);
        }
        found = 0;
        break;
    }

    freeifaddrs(ifaddr);
    return found;
}

static int
find_dpdk_port_by_mac(const struct rte_ether_addr *target_mac, uint16_t *port_id)
{
    uint16_t port;

    RTE_ETH_FOREACH_DEV(port) {
        struct rte_ether_addr mac;

        rte_eth_macaddr_get(port, &mac);
        if (rte_is_same_ether_addr(&mac, target_mac)) {
            *port_id = port;
            return 0;
        }
    }

    return -1;
}

static int
port_init(uint16_t port_id, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf;
    struct rte_eth_rxconf rx_conf;
    struct rte_eth_txconf tx_conf;
    struct rte_eth_dev_info dev_info;
    int ret;

    memset(&port_conf, 0, sizeof(port_conf));

    if (!rte_eth_dev_is_valid_port(port_id)) {
        return -1;
    }

    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret < 0) {
        return ret;
    }

    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0) {
        return ret;
    }

    rx_conf = dev_info.default_rxconf;
    tx_conf = dev_info.default_txconf;

    ret = rte_eth_rx_queue_setup(
        port_id,
        0,
        RX_RING_SIZE,
        rte_eth_dev_socket_id(port_id),
        &rx_conf,
        mbuf_pool
    );
    if (ret < 0) {
        return ret;
    }

    ret = rte_eth_tx_queue_setup(
        port_id,
        0,
        TX_RING_SIZE,
        rte_eth_dev_socket_id(port_id),
        &tx_conf
    );
    if (ret < 0) {
        return ret;
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

static inline int
prepare_echo_packet(struct rte_mbuf *m, uint32_t local_ip_be)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct rte_ether_addr tmp_mac;
    rte_be32_t tmp_ip;
    rte_be16_t tmp_port;
    uint16_t ether_type;

    if (rte_pktmbuf_pkt_len(m) < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                                sizeof(struct rte_udp_hdr)) {
        return -1;
    }

    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);
    if (ether_type != RTE_ETHER_TYPE_IPV4) {
        return -1;
    }

    ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    if ((ipv4_hdr->version_ihl >> 4) != 4) {
        return -1;
    }
    if ((ipv4_hdr->version_ihl & 0x0f) != 5) {
        return -1;
    }
    if (ipv4_hdr->next_proto_id != IPPROTO_UDP) {
        return -1;
    }
    if (ipv4_hdr->dst_addr != local_ip_be) {
        return -1;
    }

    udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);

    rte_ether_addr_copy(&eth_hdr->src_addr, &tmp_mac);
    rte_ether_addr_copy(&eth_hdr->dst_addr, &eth_hdr->src_addr);
    rte_ether_addr_copy(&tmp_mac, &eth_hdr->dst_addr);

    tmp_ip = ipv4_hdr->src_addr;
    ipv4_hdr->src_addr = ipv4_hdr->dst_addr;
    ipv4_hdr->dst_addr = tmp_ip;

    tmp_port = udp_hdr->src_port;
    udp_hdr->src_port = udp_hdr->dst_port;
    udp_hdr->dst_port = tmp_port;

    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->hdr_checksum = 0;
    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

    return 0;
}

static void
run_echo_loop(const struct app_config *cfg)
{
    struct rte_mbuf *rx_bufs[BURST_SIZE];
    struct rte_mbuf *tx_bufs[BURST_SIZE];
    uint64_t rx_packets = 0;
    uint64_t echoed_packets = 0;
    uint64_t dropped_packets = 0;

    while (!force_quit) {
        uint16_t nb_rx;
        uint16_t nb_tx;
        uint16_t nb_ready = 0;
        uint16_t i;

        nb_rx = rte_eth_rx_burst(cfg->port_id, cfg->queue_id, rx_bufs, BURST_SIZE);
        if (nb_rx == 0) {
            continue;
        }

        rx_packets += nb_rx;

        for (i = 0; i < nb_rx; i++) {
            if (prepare_echo_packet(rx_bufs[i], cfg->local_ip_be) == 0) {
                tx_bufs[nb_ready++] = rx_bufs[i];
            } else {
                rte_pktmbuf_free(rx_bufs[i]);
                dropped_packets++;
            }
        }

        if (nb_ready == 0) {
            continue;
        }

        nb_tx = rte_eth_tx_burst(cfg->port_id, cfg->queue_id, tx_bufs, nb_ready);
        echoed_packets += nb_tx;

        if (nb_tx < nb_ready) {
            for (i = nb_tx; i < nb_ready; i++) {
                rte_pktmbuf_free(tx_bufs[i]);
                dropped_packets++;
            }
        }
    }

    printf("rx=%" PRIu64 " echoed=%" PRIu64 " dropped=%" PRIu64 "\n",
           rx_packets, echoed_packets, dropped_packets);
}

int
main(int argc, char **argv)
{
    struct app_config cfg;
    struct rte_mempool *mbuf_pool;
    struct rte_ether_addr local_mac;
    char ifname[IF_NAMESIZE];
    char *eal_argv[] = {argv[0], NULL};
    struct in_addr local_ip;
    int ret;

    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.queue_id = 0;

    if (parse_ipv4_arg(argv[1], &cfg.local_ip_be) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (find_interface_mac_by_ip(cfg.local_ip_be, &local_mac, ifname, sizeof(ifname)) != 0) {
        rte_exit(EXIT_FAILURE, "Could not find a Linux interface with IP %s\n", argv[1]);
    }

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ret = rte_eal_init(1, eal_argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Failed to initialize DPDK EAL\n");
    }
    if (find_dpdk_port_by_mac(&local_mac, &cfg.port_id) != 0) {
        rte_exit(EXIT_FAILURE,
                 "Could not map interface %s to a DPDK port; refusing to touch other NICs\n",
                 ifname);
    }

    mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        NUM_MBUFS,
        MBUF_CACHE_SIZE,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id()
    );
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to create mbuf pool\n");
    }

    if (port_init(cfg.port_id, mbuf_pool) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to initialize port %" PRIu16 "\n", cfg.port_id);
    }

    local_ip.s_addr = cfg.local_ip_be;
    printf("Echo server using interface %s, port %" PRIu16 ", IP %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           ifname,
           cfg.port_id,
           inet_ntoa(local_ip),
           local_mac.addr_bytes[0], local_mac.addr_bytes[1], local_mac.addr_bytes[2],
           local_mac.addr_bytes[3], local_mac.addr_bytes[4], local_mac.addr_bytes[5]);

    run_echo_loop(&cfg);

    rte_eth_dev_stop(cfg.port_id);
    rte_eth_dev_close(cfg.port_id);
    return EXIT_SUCCESS;
}

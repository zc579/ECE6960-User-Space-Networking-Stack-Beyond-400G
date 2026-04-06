#include <arpa/inet.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_arp.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#define NUM_MBUFS 16384
#define MBUF_CACHE_SIZE 256
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define BURST_SIZE 32
#define UDP_PORT 9000
#define RTT_SAMPLES 10000
#define THROUGHPUT_BURSTS 4096
#define THROUGHPUT_SECONDS 3

struct payload {
    uint32_t magic;
    uint32_t seq;
    uint64_t tx_tsc;
} __attribute__((packed));

struct app_config {
    uint16_t packet_size;
    uint32_t client_ip_be;
    uint32_t server_ip_be;
    struct rte_ether_addr server_mac;
    uint16_t port_id;
    uint16_t queue_id;
};

static volatile bool force_quit;

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
    fprintf(stderr,
            "Usage: sudo %s <packet-size> <client-ip> <server-ip> <server-mac>\n",
            progname);
    fprintf(stderr,
            "Example: sudo %s 64 10.16.1.2 10.16.1.1 aa:bb:cc:dd:ee:ff\n",
            progname);
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
parse_mac_arg(const char *arg, struct rte_ether_addr *mac)
{
    unsigned int bytes[RTE_ETHER_ADDR_LEN];
    int i;

    if (sscanf(arg, "%x:%x:%x:%x:%x:%x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return -1;
    }

    for (i = 0; i < RTE_ETHER_ADDR_LEN; i++) {
        mac->addr_bytes[i] = (uint8_t)bytes[i];
    }

    return 0;
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

    ret = rte_eth_rx_queue_setup(port_id, 0, RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), &rx_conf, mbuf_pool);
    if (ret < 0) {
        return ret;
    }

    ret = rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), &tx_conf);
    if (ret < 0) {
        return ret;
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        return ret;
    }

    rte_eth_promiscuous_enable(port_id);
    return 0;
}

static struct rte_mbuf *
build_packet(struct rte_mempool *pool,
             const struct app_config *cfg,
             const struct rte_ether_addr *src_mac,
             uint32_t seq,
             uint64_t tx_tsc)
{
    struct rte_mbuf *m;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct payload *pl;
    uint16_t payload_len;
    uint16_t total_len;

    m = rte_pktmbuf_alloc(pool);
    if (m == NULL) {
        return NULL;
    }

    total_len = cfg->packet_size;
    payload_len = total_len - sizeof(struct rte_ether_hdr)
                  - sizeof(struct rte_ipv4_hdr)
                  - sizeof(struct rte_udp_hdr);

    if (rte_pktmbuf_append(m, total_len) == NULL) {
        rte_pktmbuf_free(m);
        return NULL;
    }

    memset(rte_pktmbuf_mtod(m, void *), 0, total_len);

    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    rte_ether_addr_copy(&cfg->server_mac, &eth_hdr->dst_addr);
    rte_ether_addr_copy(src_mac, &eth_hdr->src_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    ipv4_hdr->version_ihl = RTE_IPV4_VHL_DEF;
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->fragment_offset = 0;
    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->next_proto_id = IPPROTO_UDP;
    ipv4_hdr->packet_id = rte_cpu_to_be_16((uint16_t)seq);
    ipv4_hdr->total_length = rte_cpu_to_be_16(total_len - sizeof(struct rte_ether_hdr));
    ipv4_hdr->src_addr = cfg->client_ip_be;
    ipv4_hdr->dst_addr = cfg->server_ip_be;
    ipv4_hdr->hdr_checksum = 0;

    udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);
    udp_hdr->src_port = rte_cpu_to_be_16(UDP_PORT);
    udp_hdr->dst_port = rte_cpu_to_be_16(UDP_PORT);
    udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_len);
    udp_hdr->dgram_cksum = 0;

    pl = (struct payload *)(udp_hdr + 1);
    pl->magic = rte_cpu_to_be_32(0xECE6960u);
    pl->seq = rte_cpu_to_be_32(seq);
    pl->tx_tsc = rte_cpu_to_be_64(tx_tsc);

    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

    return m;
}

static bool
packet_matches(const struct rte_mbuf *m,
               const struct app_config *cfg,
               const struct rte_ether_addr *local_mac,
               uint32_t *seq,
               uint64_t *tx_tsc)
{
    const struct rte_ether_hdr *eth_hdr;
    const struct rte_ipv4_hdr *ipv4_hdr;
    const struct rte_udp_hdr *udp_hdr;
    const struct payload *pl;
    uint16_t ether_type;

    if (rte_pktmbuf_pkt_len(m) < cfg->packet_size) {
        return false;
    }

    eth_hdr = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);
    if (ether_type != RTE_ETHER_TYPE_IPV4) {
        return false;
    }
    if (!rte_is_same_ether_addr(&eth_hdr->src_addr, &cfg->server_mac)) {
        return false;
    }
    if (!rte_is_same_ether_addr(&eth_hdr->dst_addr, local_mac)) {
        return false;
    }

    ipv4_hdr = (const struct rte_ipv4_hdr *)(eth_hdr + 1);
    if (ipv4_hdr->next_proto_id != IPPROTO_UDP) {
        return false;
    }
    if (ipv4_hdr->src_addr != cfg->server_ip_be || ipv4_hdr->dst_addr != cfg->client_ip_be) {
        return false;
    }

    udp_hdr = (const struct rte_udp_hdr *)(ipv4_hdr + 1);
    if (udp_hdr->src_port != rte_cpu_to_be_16(UDP_PORT) ||
        udp_hdr->dst_port != rte_cpu_to_be_16(UDP_PORT)) {
        return false;
    }

    pl = (const struct payload *)(udp_hdr + 1);
    if (pl->magic != rte_cpu_to_be_32(0xECE6960u)) {
        return false;
    }

    *seq = rte_be_to_cpu_32(pl->seq);
    *tx_tsc = rte_be_to_cpu_64(pl->tx_tsc);
    return true;
}

static int
wait_for_echo(uint16_t port_id,
              uint16_t queue_id,
              const struct app_config *cfg,
              const struct rte_ether_addr *local_mac,
              uint32_t expected_seq,
              uint64_t *rtt_cycles)
{
    struct rte_mbuf *rx_bufs[BURST_SIZE];
    uint64_t deadline;

    deadline = rte_get_timer_cycles() + rte_get_timer_hz();

    while (!force_quit && rte_get_timer_cycles() < deadline) {
        uint16_t nb_rx;
        uint16_t i;

        nb_rx = rte_eth_rx_burst(port_id, queue_id, rx_bufs, BURST_SIZE);
        if (nb_rx == 0) {
            continue;
        }

        for (i = 0; i < nb_rx; i++) {
            uint32_t seq;
            uint64_t tx_tsc;
            bool ok;

            ok = packet_matches(rx_bufs[i], cfg, local_mac, &seq, &tx_tsc);
            if (ok && seq == expected_seq) {
                *rtt_cycles = rte_get_timer_cycles() - tx_tsc;
                rte_pktmbuf_free(rx_bufs[i]);
                while (++i < nb_rx) {
                    rte_pktmbuf_free(rx_bufs[i]);
                }
                return 0;
            }

            rte_pktmbuf_free(rx_bufs[i]);
        }
    }

    return -1;
}

static void
measure_rtt(struct rte_mempool *pool,
            const struct app_config *cfg,
            const struct rte_ether_addr *local_mac)
{
    uint64_t hz = rte_get_timer_hz();
    uint64_t min_cycles = UINT64_MAX;
    uint64_t max_cycles = 0;
    uint64_t total_cycles = 0;
    uint32_t success = 0;
    uint32_t seq;

    for (seq = 1; seq <= RTT_SAMPLES && !force_quit; seq++) {
        struct rte_mbuf *m;
        uint64_t tx_tsc;
        uint64_t rtt_cycles;

        tx_tsc = rte_get_timer_cycles();
        m = build_packet(pool, cfg, local_mac, seq, tx_tsc);
        if (m == NULL) {
            continue;
        }

        if (rte_eth_tx_burst(cfg->port_id, cfg->queue_id, &m, 1) != 1) {
            rte_pktmbuf_free(m);
            continue;
        }

        if (wait_for_echo(cfg->port_id, cfg->queue_id, cfg, local_mac, seq, &rtt_cycles) == 0) {
            if (rtt_cycles < min_cycles) {
                min_cycles = rtt_cycles;
            }
            if (rtt_cycles > max_cycles) {
                max_cycles = rtt_cycles;
            }
            total_cycles += rtt_cycles;
            success++;
        }
    }

    if (success == 0) {
        printf("RTT measurement failed: no valid echoes received\n");
        return;
    }

    printf("RTT samples=%u avg=%.3f us min=%.3f us max=%.3f us\n",
           success,
           (double)total_cycles / success * 1e6 / hz,
           (double)min_cycles * 1e6 / hz,
           (double)max_cycles * 1e6 / hz);
}

static void
measure_throughput(struct rte_mempool *pool,
                   const struct app_config *cfg,
                   const struct rte_ether_addr *local_mac)
{
    uint64_t hz = rte_get_timer_hz();
    uint64_t start = rte_get_timer_cycles();
    uint64_t end = start + hz * THROUGHPUT_SECONDS;
    uint64_t tx_packets = 0;
    uint64_t rx_packets = 0;
    uint32_t seq = RTT_SAMPLES + 1;

    while (!force_quit && rte_get_timer_cycles() < end) {
        struct rte_mbuf *tx_bufs[BURST_SIZE];
        uint16_t nb_ready = 0;
        uint16_t nb_tx;
        uint16_t i;

        for (i = 0; i < BURST_SIZE && seq < RTT_SAMPLES + THROUGHPUT_BURSTS * BURST_SIZE; i++, seq++) {
            tx_bufs[nb_ready] = build_packet(pool, cfg, local_mac, seq, 0);
            if (tx_bufs[nb_ready] != NULL) {
                nb_ready++;
            }
        }

        if (nb_ready == 0) {
            continue;
        }

        nb_tx = rte_eth_tx_burst(cfg->port_id, cfg->queue_id, tx_bufs, nb_ready);
        tx_packets += nb_tx;
        if (nb_tx < nb_ready) {
            for (i = nb_tx; i < nb_ready; i++) {
                rte_pktmbuf_free(tx_bufs[i]);
            }
        }

        {
            struct rte_mbuf *rx_bufs[BURST_SIZE];
            uint16_t nb_rx;

            nb_rx = rte_eth_rx_burst(cfg->port_id, cfg->queue_id, rx_bufs, BURST_SIZE);
            for (i = 0; i < nb_rx; i++) {
                uint32_t rx_seq;
                uint64_t tx_tsc;

                if (packet_matches(rx_bufs[i], cfg, local_mac, &rx_seq, &tx_tsc)) {
                    rx_packets++;
                }
                rte_pktmbuf_free(rx_bufs[i]);
            }
        }
    }

    {
        double seconds = (double)THROUGHPUT_SECONDS;
        double rx_mpps = rx_packets / seconds / 1e6;
        double rx_gbps = rx_packets * cfg->packet_size * 8.0 / seconds / 1e9;

        printf("Throughput duration=%d s tx=%" PRIu64 " rx=%" PRIu64
               " rx_mpps=%.3f rx_gbps=%.3f\n",
               THROUGHPUT_SECONDS, tx_packets, rx_packets, rx_mpps, rx_gbps);
    }
}

int
main(int argc, char **argv)
{
    struct app_config cfg;
    struct rte_mempool *mbuf_pool;
    struct rte_ether_addr local_mac;
    char *eal_argv[] = {argv[0], NULL};
    int ret;

    if (argc != 5) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.port_id = 0;
    cfg.queue_id = 0;
    cfg.packet_size = (uint16_t)atoi(argv[1]);

    if (cfg.packet_size < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                              sizeof(struct rte_udp_hdr) + sizeof(struct payload)) {
        fprintf(stderr, "Packet size too small\n");
        return EXIT_FAILURE;
    }
    if (cfg.packet_size > RTE_ETHER_MAX_LEN) {
        fprintf(stderr, "Packet size too large\n");
        return EXIT_FAILURE;
    }
    if (parse_ipv4_arg(argv[2], &cfg.client_ip_be) != 0 ||
        parse_ipv4_arg(argv[3], &cfg.server_ip_be) != 0 ||
        parse_mac_arg(argv[4], &cfg.server_mac) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ret = rte_eal_init(1, eal_argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Failed to initialize DPDK EAL\n");
    }

    mbuf_pool = rte_pktmbuf_pool_create("CLIENT_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to create mbuf pool\n");
    }

    if (port_init(cfg.port_id, mbuf_pool) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to initialize port %" PRIu16 "\n", cfg.port_id);
    }

    rte_eth_macaddr_get(cfg.port_id, &local_mac);

    printf("Starting packet_gen_client packet_size=%u\n", cfg.packet_size);
    measure_rtt(mbuf_pool, &cfg, &local_mac);
    measure_throughput(mbuf_pool, &cfg, &local_mac);

    rte_eth_dev_stop(cfg.port_id);
    rte_eth_dev_close(cfg.port_id);
    return EXIT_SUCCESS;
}

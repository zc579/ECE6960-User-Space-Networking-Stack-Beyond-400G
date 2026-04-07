#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include "dpdk.h"

#define REPORT_INTERVAL_SEC 1

struct echo_profile {
    uint64_t rx_bursts;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t tx_drops;
    uint64_t rx_cycles;
    uint64_t parse_cycles;
    uint64_t rewrite_cycles;
    uint64_t checksum_cycles;
    uint64_t tx_cycles;
};

static double
cycles_per_packet(uint64_t cycles, uint64_t packets)
{
    if (packets == 0) {
        return 0.0;
    }

    return (double)cycles / (double)packets;
}

static double
cycles_per_burst(uint64_t cycles, uint64_t bursts)
{
    if (bursts == 0) {
        return 0.0;
    }

    return (double)cycles / (double)bursts;
}

static void
print_profile(const struct echo_profile *stats, uint64_t hz)
{
    double rx_mpps = (double)stats->rx_packets / (double)REPORT_INTERVAL_SEC / 1000000.0;
    double tx_mpps = (double)stats->tx_packets / (double)REPORT_INTERVAL_SEC / 1000000.0;
    double avg_burst = stats->rx_bursts == 0
                           ? 0.0
                           : (double)stats->rx_packets / (double)stats->rx_bursts;

    printf("[profile] rx_pkts=%" PRIu64 " tx_pkts=%" PRIu64
           " tx_drops=%" PRIu64 " rx_mpps=%.3f tx_mpps=%.3f avg_burst=%.2f"
           " rx_cycles/burst=%.1f rx_cycles/pkt=%.1f parse_cycles/pkt=%.1f"
           " rewrite_cycles/pkt=%.1f checksum_cycles/pkt=%.1f tx_cycles/pkt=%.1f"
           " cpu_ghz=%.3f\n",
           stats->rx_packets,
           stats->tx_packets,
           stats->tx_drops,
           rx_mpps,
           tx_mpps,
           avg_burst,
           cycles_per_burst(stats->rx_cycles, stats->rx_bursts),
           cycles_per_packet(stats->rx_cycles, stats->rx_packets),
           cycles_per_packet(stats->parse_cycles, stats->rx_packets),
           cycles_per_packet(stats->rewrite_cycles, stats->rx_packets),
           cycles_per_packet(stats->checksum_cycles, stats->rx_packets),
           cycles_per_packet(stats->tx_cycles, stats->tx_packets),
           (double)hz / 1000000000.0);
}

/*
 * Run an echo server.
 */
static int
run_server(void)
{
    uint8_t port = dpdk_port;
    struct rte_mbuf *rx_bufs[BURST_SIZE];
    struct echo_profile stats = {0};
    uint64_t hz = rte_get_timer_hz();
    uint64_t last_report = rte_get_timer_cycles();
    uint64_t report_cycles = hz * REPORT_INTERVAL_SEC;

    printf("\nCore %u running in server mode. [Ctrl+C to quit]\n",
           rte_lcore_id());

    /* Run until the application is quit or killed. */
    for (;;) {
        uint16_t nb_rx;
        uint64_t t0;
        uint64_t t1;

        /* Receive packets. */
        t0 = rte_get_timer_cycles();
        nb_rx = rte_eth_rx_burst(port, 0, rx_bufs, BURST_SIZE);
        t1 = rte_get_timer_cycles();

        stats.rx_bursts++;
        stats.rx_cycles += t1 - t0;

        if (nb_rx == 0) {
            uint64_t now = rte_get_timer_cycles();

            if (now - last_report >= report_cycles) {
                print_profile(&stats, hz);
                memset(&stats, 0, sizeof(stats));
                last_report = now;
            }
            continue;
        }

        stats.rx_packets += nb_rx;

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *buf = rx_bufs[i];
            struct rte_ether_hdr *eth_hdr;
            struct rte_ipv4_hdr *ipv4_hdr;
            struct rte_udp_hdr *udp_hdr;
            struct rte_ether_addr tmp_eth_addr;
            rte_be32_t tmp_ip_addr;
            rte_be16_t tmp_udp_port;

            t0 = rte_get_timer_cycles();
            eth_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
            ipv4_hdr = rte_pktmbuf_mtod_offset(
                buf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
            udp_hdr = rte_pktmbuf_mtod_offset(
                buf,
                struct rte_udp_hdr *,
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
            t1 = rte_get_timer_cycles();
            stats.parse_cycles += t1 - t0;

            /* Swap Ethernet source and destination MAC addresses. */
            t0 = rte_get_timer_cycles();
            rte_ether_addr_copy(&eth_hdr->src_addr, &tmp_eth_addr);
            rte_ether_addr_copy(&eth_hdr->dst_addr, &eth_hdr->src_addr);
            rte_ether_addr_copy(&tmp_eth_addr, &eth_hdr->dst_addr);

            /* Swap IPv4 source and destination addresses. */
            tmp_ip_addr = ipv4_hdr->src_addr;
            ipv4_hdr->src_addr = ipv4_hdr->dst_addr;
            ipv4_hdr->dst_addr = tmp_ip_addr;

            /* Swap UDP source and destination ports. */
            tmp_udp_port = udp_hdr->src_port;
            udp_hdr->src_port = udp_hdr->dst_port;
            udp_hdr->dst_port = tmp_udp_port;
            t1 = rte_get_timer_cycles();
            stats.rewrite_cycles += t1 - t0;

            /* Recompute checksums after modifying packet headers. */
            t0 = rte_get_timer_cycles();
            ipv4_hdr->hdr_checksum = 0;
            ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

            udp_hdr->dgram_cksum = 0;
            udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
            t1 = rte_get_timer_cycles();
            stats.checksum_cycles += t1 - t0;

            /* Send the packet back. */
            t0 = rte_get_timer_cycles();
            if (rte_eth_tx_burst(port, 0, &buf, 1) != 1) {
                rte_pktmbuf_free(buf);
                stats.tx_drops++;
            } else {
                stats.tx_packets++;
            }
            t1 = rte_get_timer_cycles();
            stats.tx_cycles += t1 - t0;
        }

        {
            uint64_t now = rte_get_timer_cycles();

            if (now - last_report >= report_cycles) {
                print_profile(&stats, hz);
                memset(&stats, 0, sizeof(stats));
                last_report = now;
            }
        }
    }

    return 0;
}

static int
parse_echo_args(int argc, char *argv[])
{
    if (argc != 2) {
        printf("argument number incorrect: %d\n", argc);
        printf("usage: sudo ./echo_server <SERVER_IP>\n");
        printf("example: sudo ./echo_server 10.16.1.1\n");
        return -EINVAL;
    }

    str_to_ip(argv[1], &my_ip);
    return 0;
}

/*
 * The main function, which does initialization and starts the server.
 */
int
main(int argc, char *argv[])
{
    int args_parsed;
    int res;

    /* Initialize DPDK. */
    args_parsed = dpdk_init(argc, argv);

    /* Initialize our arguments. */
    argc -= args_parsed;
    argv += args_parsed;

    res = parse_echo_args(argc, argv);
    if (res < 0) {
        return 0;
    }

    /* Initialize port. */
    if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0) {
        rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu8 "\n", dpdk_port);
    }

    run_server();
    return 0;
}

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include "dpdk.h"

/*
 * Run an echo server.
 */
static int
run_server(void)
{
    uint8_t port = dpdk_port;
    struct rte_mbuf *rx_bufs[BURST_SIZE];

    printf("\nCore %u running in server mode. [Ctrl+C to quit]\n",
           rte_lcore_id());

    /* Run until the application is quit or killed. */
    for (;;) {
        uint16_t nb_rx;

        /* Receive packets. */
        nb_rx = rte_eth_rx_burst(port, 0, rx_bufs, BURST_SIZE);
        if (nb_rx == 0) {
            continue;
        }

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *buf = rx_bufs[i];
            struct rte_ether_hdr *eth_hdr;
            struct rte_ipv4_hdr *ipv4_hdr;
            struct rte_udp_hdr *udp_hdr;
            struct rte_ether_addr tmp_eth_addr;
            rte_be32_t tmp_ip_addr;
            rte_be16_t tmp_udp_port;

            eth_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
            ipv4_hdr = rte_pktmbuf_mtod_offset(
                buf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
            udp_hdr = rte_pktmbuf_mtod_offset(
                buf,
                struct rte_udp_hdr *,
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

            /* Swap Ethernet source and destination MAC addresses. */
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

            /* Recompute checksums after modifying packet headers. */
            ipv4_hdr->hdr_checksum = 0;
            ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

            udp_hdr->dgram_cksum = 0;
            udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

            /* Send the packet back. */
            if (rte_eth_tx_burst(port, 0, &buf, 1) != 1) {
                rte_pktmbuf_free(buf);
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

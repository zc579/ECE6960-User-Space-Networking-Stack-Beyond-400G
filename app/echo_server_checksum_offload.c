#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
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

#define port_init dpdk_default_port_init
#include "dpdk.h"
#undef port_init

#define REPORT_INTERVAL_SEC 1

#ifndef RTE_ETH_TX_OFFLOAD_IPV4_CKSUM
#define RTE_ETH_TX_OFFLOAD_IPV4_CKSUM DEV_TX_OFFLOAD_IPV4_CKSUM
#endif
#ifndef RTE_ETH_TX_OFFLOAD_UDP_CKSUM
#define RTE_ETH_TX_OFFLOAD_UDP_CKSUM DEV_TX_OFFLOAD_UDP_CKSUM
#endif
#ifndef RTE_MBUF_F_TX_IPV4
#define RTE_MBUF_F_TX_IPV4 PKT_TX_IPV4
#endif
#ifndef RTE_MBUF_F_TX_IP_CKSUM
#define RTE_MBUF_F_TX_IP_CKSUM PKT_TX_IP_CKSUM
#endif
#ifndef RTE_MBUF_F_TX_UDP_CKSUM
#define RTE_MBUF_F_TX_UDP_CKSUM PKT_TX_UDP_CKSUM
#endif

enum checksum_mode {
    CHECKSUM_MODE_SOFTWARE,
    CHECKSUM_MODE_OFFLOAD,
    CHECKSUM_MODE_NONE,
};

static enum checksum_mode selected_checksum_mode = CHECKSUM_MODE_SOFTWARE;
static uint64_t requested_tx_offloads;
static uint64_t supported_tx_offloads;
static uint64_t enabled_tx_offloads;

static const char *
checksum_mode_name(enum checksum_mode mode)
{
    switch (mode) {
    case CHECKSUM_MODE_SOFTWARE:
        return "software";
    case CHECKSUM_MODE_OFFLOAD:
        return "offload";
    case CHECKSUM_MODE_NONE:
        return "none";
    default:
        return "unknown";
    }
}

struct worker_ctx {
    uint8_t queue_id;
    unsigned int lcore_id;
};

struct echo_profile {
    uint64_t poll_count;
    uint64_t empty_poll_count;
    uint64_t nonempty_poll_count;
    uint64_t work_batches;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t tx_drops;
    uint64_t total_loop_cycles;
    uint64_t empty_poll_cycles;
    uint64_t nonempty_poll_cycles;
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
cycles_per_event(uint64_t cycles, uint64_t count)
{
    if (count == 0) {
        return 0.0;
    }

    return (double)cycles / (double)count;
}

static void
print_profile(const struct echo_profile *stats,
              uint64_t hz,
              const struct worker_ctx *ctx)
{
    /* Non-empty-batch-normalized stage metrics use non-empty polls as the
     * useful-batch denominator. This keeps avg_burst and
     * cycles/nonempty_batch aligned to batches that actually carried packets,
     * while cycles/pkt amortizes everything over received packets.
     *
     * Poll-normalized metrics are printed separately so idle periods still
     * show a meaningful per-poll cost even when rx_pkts == 0.
     */
    uint64_t accounted_cycles = stats->rx_cycles + stats->parse_cycles +
                                stats->rewrite_cycles + stats->checksum_cycles +
                                stats->tx_cycles;
    /* Clamp at zero in case timer-read noise ever makes the summed explicit
     * stage timers slightly exceed total_loop_cycles in a short interval.
     */
    uint64_t other_cycles = stats->total_loop_cycles > accounted_cycles
                                ? stats->total_loop_cycles - accounted_cycles
                                : 0;
    double rx_mpps = (double)stats->rx_packets / (double)REPORT_INTERVAL_SEC / 1000000.0;
    double tx_mpps = (double)stats->tx_packets / (double)REPORT_INTERVAL_SEC / 1000000.0;
    double avg_burst = cycles_per_event(stats->rx_packets,
                                        stats->nonempty_poll_count);
    double pkts_per_nonempty_poll = avg_burst;
    double empty_poll_ratio = cycles_per_event(stats->empty_poll_count,
                                               stats->poll_count);

    printf("[profile] queue_id=%u lcore=%u"
           " rx_pkts=%" PRIu64 " tx_pkts=%" PRIu64
           " tx_drops=%" PRIu64
           " poll_count=%" PRIu64
           " empty_poll_count=%" PRIu64
           " nonempty_poll_count=%" PRIu64
           " work_batches=%" PRIu64
           " rx_mpps=%.3f tx_mpps=%.3f"
           " avg_burst=%.2f pkts_per_nonempty_poll=%.2f"
           " empty_poll_ratio=%.4f"
           " total_cycles/poll=%.1f"
           " total_cycles/nonempty_batch=%.1f total_cycles/pkt=%.1f"
           " empty_poll_cycles/nonempty_batch=%.1f"
           " empty_poll_cycles/empty_poll=%.1f empty_poll_cycles/pkt=%.1f"
           " nonempty_poll_cycles/nonempty_batch=%.1f"
           " nonempty_poll_cycles/nonempty_poll=%.1f nonempty_poll_cycles/pkt=%.1f"
           " rx_cycles/nonempty_batch=%.1f rx_cycles/pkt=%.1f"
           " parse_cycles/nonempty_batch=%.1f parse_cycles/pkt=%.1f"
           " rewrite_cycles/nonempty_batch=%.1f rewrite_cycles/pkt=%.1f"
           " checksum_cycles/nonempty_batch=%.1f checksum_cycles/pkt=%.1f"
           " tx_cycles/nonempty_batch=%.1f tx_cycles/pkt=%.1f"
           " other_cycles/nonempty_batch=%.1f other_cycles/pkt=%.1f"
           " cpu_ghz=%.3f\n",
           ctx->queue_id,
           ctx->lcore_id,
           stats->rx_packets,
           stats->tx_packets,
           stats->tx_drops,
           stats->poll_count,
           stats->empty_poll_count,
           stats->nonempty_poll_count,
           stats->work_batches,
           rx_mpps,
           tx_mpps,
           avg_burst,
           pkts_per_nonempty_poll,
           empty_poll_ratio,
           cycles_per_event(stats->total_loop_cycles, stats->poll_count),
           cycles_per_event(stats->total_loop_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->total_loop_cycles, stats->rx_packets),
           cycles_per_event(stats->empty_poll_cycles, stats->nonempty_poll_count),
           cycles_per_event(stats->empty_poll_cycles, stats->empty_poll_count),
           cycles_per_packet(stats->empty_poll_cycles, stats->rx_packets),
           cycles_per_event(stats->nonempty_poll_cycles, stats->nonempty_poll_count),
           cycles_per_event(stats->nonempty_poll_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->nonempty_poll_cycles, stats->rx_packets),
           cycles_per_event(stats->rx_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->rx_cycles, stats->rx_packets),
           cycles_per_event(stats->parse_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->parse_cycles, stats->rx_packets),
           cycles_per_event(stats->rewrite_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->rewrite_cycles, stats->rx_packets),
           cycles_per_event(stats->checksum_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->checksum_cycles, stats->rx_packets),
           cycles_per_event(stats->tx_cycles, stats->nonempty_poll_count),
           cycles_per_packet(stats->tx_cycles, stats->tx_packets),
           cycles_per_event(other_cycles, stats->nonempty_poll_count),
           cycles_per_packet(other_cycles, stats->rx_packets),
           (double)hz / 1000000000.0);
}

static int
port_init_checksum(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_queues)
{
    struct rte_eth_conf port_conf = port_conf_default;
    const uint16_t rx_rings = n_queues, tx_rings = n_queues;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_rxconf rxconf;
    struct rte_eth_txconf txconf;

    printf("initializing port %u with %u queues\n", port, n_queues);
    printf("Checksum mode selected: %s\n",
           checksum_mode_name(selected_checksum_mode));

    if (!rte_eth_dev_is_valid_port(port)) {
        return -1;
    }

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        return retval;
    }

    supported_tx_offloads = dev_info.tx_offload_capa;
    requested_tx_offloads = 0;
    enabled_tx_offloads = 0;

    if (selected_checksum_mode == CHECKSUM_MODE_OFFLOAD) {
        requested_tx_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                                RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
        if ((supported_tx_offloads & requested_tx_offloads) !=
            requested_tx_offloads) {
            rte_exit(EXIT_FAILURE,
                     "Checksum offload requested but not supported on port %u: "
                     "requested=0x%" PRIx64 " supported=0x%" PRIx64
                     " missing=0x%" PRIx64 "\n",
                     (unsigned)port,
                     requested_tx_offloads,
                     supported_tx_offloads,
                     requested_tx_offloads & ~supported_tx_offloads);
        }

        port_conf.txmode.offloads |= requested_tx_offloads;
        enabled_tx_offloads = port_conf.txmode.offloads;
    }

    printf("TX offloads: requested=0x%" PRIx64
           " supported=0x%" PRIx64
           " enabled=0x%" PRIx64 "\n",
           requested_tx_offloads,
           supported_tx_offloads,
           enabled_tx_offloads);

    if (n_queues > 1) {
        uint64_t rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_UDP;

        rss_hf &= dev_info.flow_type_rss_offloads;
        if (rss_hf == 0) {
            rte_exit(EXIT_FAILURE,
                     "RSS for IPv4/UDP is not supported on port %u\n",
                     port);
        }

        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        printf("Port %u RSS enabled: rss_hf=0x%" PRIx64
               " reta_size=%u hash_key_size=%u\n",
               (unsigned)port,
               rss_hf,
               dev_info.reta_size,
               dev_info.hash_key_size);
    }

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0) {
        return retval;
    }

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0) {
        return retval;
    }

    rxconf = dev_info.default_rxconf;
    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;

    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port),
                                        &rxconf, mbuf_pool);
        if (retval < 0) {
            return retval;
        }
    }

    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port),
                                        &txconf);
        if (retval < 0) {
            return retval;
        }
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0) {
        return retval;
    }

    rte_eth_macaddr_get(port, &my_eth);
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
           " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
           (unsigned)port,
           my_eth.addr_bytes[0], my_eth.addr_bytes[1],
           my_eth.addr_bytes[2], my_eth.addr_bytes[3],
           my_eth.addr_bytes[4], my_eth.addr_bytes[5]);

    rte_eth_promiscuous_enable(port);

    return 0;
}

/*
 * Run one symmetric echo worker pinned to a specific RX/TX queue.
 */
static int
run_server_worker(void *arg)
{
    const struct worker_ctx *ctx = arg;
    uint8_t port = dpdk_port;
    uint16_t queue_id = ctx->queue_id;
    struct rte_mbuf *rx_bufs[BURST_SIZE];
    struct rte_ether_hdr *eth_hdrs[BURST_SIZE];
    struct rte_ipv4_hdr *ipv4_hdrs[BURST_SIZE];
    struct rte_udp_hdr *udp_hdrs[BURST_SIZE];
    struct echo_profile stats = {0};
    uint64_t hz = rte_get_timer_hz();
    uint64_t last_report = rte_get_timer_cycles();
    uint64_t report_cycles = hz * REPORT_INTERVAL_SEC;

    printf("\nCore %u running in server mode on queue %u. [Ctrl+C to quit]\n",
           rte_lcore_id(), queue_id);

    /* Run until the application is quit or killed. */
    for (;;) {
        uint16_t nb_rx;
        uint64_t loop_start;
        uint64_t loop_end;
        uint64_t loop_cycles;
        uint64_t t0;
        uint64_t t1;

        /* Measure the hot loop iteration as one unit so we can compare
         * stage timers against the full per-iteration cost later.
         *
         * We intentionally stop this timer before print_profile()/memset()
         * to avoid console I/O dominating the datapath measurements.
         */
        loop_start = rte_get_timer_cycles();

        /* Receive packets. */
        t0 = rte_get_timer_cycles();
        nb_rx = rte_eth_rx_burst(port, queue_id, rx_bufs, BURST_SIZE);
        t1 = rte_get_timer_cycles();

        stats.poll_count++;
        stats.rx_cycles += t1 - t0;

        if (nb_rx == 0) {
            /* Empty-poll cycles include the whole iteration cost for a poll
             * that did not return packets: RX call, branch/control flow,
             * timer reads, and report-check bookkeeping in this path.
             */
            loop_end = rte_get_timer_cycles();
            loop_cycles = loop_end - loop_start;
            stats.total_loop_cycles += loop_cycles;
            stats.empty_poll_count++;
            stats.empty_poll_cycles += loop_cycles;

            if (loop_end - last_report >= report_cycles) {
                print_profile(&stats, hz, ctx);
                memset(&stats, 0, sizeof(stats));
                last_report = loop_end;
            }
            continue;
        }

        stats.nonempty_poll_count++;
        stats.work_batches++;
        stats.rx_packets += nb_rx;

        t0 = rte_get_timer_cycles();
        for (uint16_t i = 0; i < nb_rx; i++) {
            eth_hdrs[i] = rte_pktmbuf_mtod(rx_bufs[i], struct rte_ether_hdr *);
            ipv4_hdrs[i] = rte_pktmbuf_mtod_offset(
                rx_bufs[i], struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
            udp_hdrs[i] = rte_pktmbuf_mtod_offset(
                rx_bufs[i],
                struct rte_udp_hdr *,
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
        }
        t1 = rte_get_timer_cycles();
        stats.parse_cycles += t1 - t0;

        t0 = rte_get_timer_cycles();
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_ether_hdr *eth_hdr = eth_hdrs[i];
            struct rte_ipv4_hdr *ipv4_hdr = ipv4_hdrs[i];
            struct rte_udp_hdr *udp_hdr = udp_hdrs[i];
            struct rte_ether_addr tmp_eth_addr;
            rte_be32_t tmp_ip_addr;
            rte_be16_t tmp_udp_port;

            /* Swap Ethernet, IPv4, and UDP endpoint fields. */
            rte_ether_addr_copy(&eth_hdr->src_addr, &tmp_eth_addr);
            rte_ether_addr_copy(&eth_hdr->dst_addr, &eth_hdr->src_addr);
            rte_ether_addr_copy(&tmp_eth_addr, &eth_hdr->dst_addr);

            tmp_ip_addr = ipv4_hdr->src_addr;
            ipv4_hdr->src_addr = ipv4_hdr->dst_addr;
            ipv4_hdr->dst_addr = tmp_ip_addr;

            tmp_udp_port = udp_hdr->src_port;
            udp_hdr->src_port = udp_hdr->dst_port;
            udp_hdr->dst_port = tmp_udp_port;
        }
        t1 = rte_get_timer_cycles();
        stats.rewrite_cycles += t1 - t0;

        t0 = rte_get_timer_cycles();
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_ipv4_hdr *ipv4_hdr = ipv4_hdrs[i];
            struct rte_udp_hdr *udp_hdr = udp_hdrs[i];

            if (selected_checksum_mode == CHECKSUM_MODE_SOFTWARE) {
                ipv4_hdr->hdr_checksum = 0;
                ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

                udp_hdr->dgram_cksum = 0;
                udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
            } else if (selected_checksum_mode == CHECKSUM_MODE_OFFLOAD) {
                struct rte_mbuf *mbuf = rx_bufs[i];

                mbuf->l2_len = RTE_ETHER_HDR_LEN;
                mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
                mbuf->l4_len = sizeof(struct rte_udp_hdr);
                mbuf->ol_flags = RTE_MBUF_F_TX_IPV4 |
                                  RTE_MBUF_F_TX_IP_CKSUM |
                                  RTE_MBUF_F_TX_UDP_CKSUM;
                ipv4_hdr->hdr_checksum = 0;
                udp_hdr->dgram_cksum = rte_ipv4_phdr_cksum(ipv4_hdr,
                                                           mbuf->ol_flags);
            }
        }
        t1 = rte_get_timer_cycles();
        stats.checksum_cycles += t1 - t0;

        t0 = rte_get_timer_cycles();
        {
            uint16_t nb_tx = rte_eth_tx_burst(port, queue_id, rx_bufs, nb_rx);

            t1 = rte_get_timer_cycles();
            stats.tx_cycles += t1 - t0;
            stats.tx_packets += nb_tx;

            if (unlikely(nb_tx < nb_rx)) {
                for (uint16_t i = nb_tx; i < nb_rx; i++) {
                    rte_pktmbuf_free(rx_bufs[i]);
                }
                stats.tx_drops += nb_rx - nb_tx;
            }
        }
        loop_end = rte_get_timer_cycles();
        loop_cycles = loop_end - loop_start;
        stats.total_loop_cycles += loop_cycles;
        /* Non-empty-poll cycles include the whole iteration cost for polls
         * that did return packets. This overlaps with RX/parse/rewrite/TX
         * stage timers by design. The derived "other_cycles" metric subtracts
         * those explicit stage timers from total_loop_cycles to estimate the
         * remaining control/housekeeping cost in the hot path.
         */
        stats.nonempty_poll_cycles += loop_cycles;

        if (loop_end - last_report >= report_cycles) {
            print_profile(&stats, hz, ctx);
            memset(&stats, 0, sizeof(stats));
            last_report = loop_end;
        }
    }

    return 0;
}

static int
parse_echo_args(int argc, char *argv[])
{
    long worker_count;
    const char *worker_arg = NULL;

    if (argc < 2 || argc > 3) {
        printf("argument number incorrect: %d\n", argc);
        printf("usage: sudo ./echo_server_checksum_offload "
               "[--checksum-mode=software|offload|none] <NUM_WORKERS>\n");
        printf("example: sudo ./echo_server_checksum_offload "
               "--checksum-mode=offload 2\n");
        return -EINVAL;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strncmp(arg, "--checksum-mode=", strlen("--checksum-mode=")) == 0) {
            const char *mode_arg = arg + strlen("--checksum-mode=");

            if (strcmp(mode_arg, "software") == 0) {
                selected_checksum_mode = CHECKSUM_MODE_SOFTWARE;
            } else if (strcmp(mode_arg, "offload") == 0) {
                selected_checksum_mode = CHECKSUM_MODE_OFFLOAD;
            } else if (strcmp(mode_arg, "none") == 0) {
                selected_checksum_mode = CHECKSUM_MODE_NONE;
            } else {
                printf("invalid checksum mode: %s\n", mode_arg);
                return -EINVAL;
            }
        } else if (worker_arg == NULL) {
            worker_arg = arg;
        } else {
            printf("unexpected argument: %s\n", arg);
            return -EINVAL;
        }
    }

    if (worker_arg == NULL) {
        printf("missing NUM_WORKERS argument\n");
        return -EINVAL;
    }

    worker_count = strtol(worker_arg, NULL, 10);
    if (worker_count <= 0 || worker_count > MAX_CORES) {
        printf("num_workers must be in range [1, %d]\n", MAX_CORES);
        return -EINVAL;
    }

    num_queues = (unsigned int)worker_count;
    my_ip = DEFAULT_SERVER_IP;
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
    struct worker_ctx *worker_ctxs;
    unsigned int used_workers = 0;
    unsigned int next_queue = 0;
    unsigned int lcore_id;

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
    if (port_init_checksum(dpdk_port, rx_mbuf_pool, num_queues) != 0) {
        rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu8 "\n", dpdk_port);
    }

    worker_ctxs = calloc(num_queues, sizeof(*worker_ctxs));
    if (worker_ctxs == NULL) {
        rte_exit(EXIT_FAILURE,
                 "Cannot allocate worker contexts for %u queues\n",
                 num_queues);
    }

    if (rte_lcore_count() < num_queues) {
        rte_exit(EXIT_FAILURE,
                 "Need at least %u lcores for %u queue workers\n",
                 num_queues,
                 num_queues);
    }

    worker_ctxs[next_queue].queue_id = next_queue;
    worker_ctxs[next_queue].lcore_id = rte_lcore_id();
    used_workers++;
    next_queue++;

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (next_queue >= num_queues) {
            break;
        }

        worker_ctxs[next_queue].queue_id = next_queue;
        worker_ctxs[next_queue].lcore_id = lcore_id;
        if (rte_eal_remote_launch(run_server_worker,
                                  &worker_ctxs[next_queue],
                                  lcore_id) != 0) {
            rte_exit(EXIT_FAILURE,
                     "Cannot launch worker for queue %u on lcore %u\n",
                     next_queue,
                     lcore_id);
        }
        used_workers++;
        next_queue++;
    }

    if (used_workers < num_queues) {
        rte_exit(EXIT_FAILURE,
                 "Only mapped %u workers for %u queues\n",
                 used_workers,
                 num_queues);
    }

    return run_server_worker(&worker_ctxs[0]);
}

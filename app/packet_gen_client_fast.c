#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_pause.h>
#include <rte_udp.h>
#include "dpdk.h"

#ifndef FAST_TX_BURSTS_PER_RX
#define FAST_TX_BURSTS_PER_RX 4
#endif

struct fast_client_worker_ctx {
	uint8_t queue_id;
	unsigned int lcore_id;
	uint64_t start_cycles;
	uint64_t end_cycles;
	uint64_t initial_flow_id;
	uint64_t completed;
	uint64_t tx_sent;
	uint64_t tx_drops;
	uint64_t alloc_failures;
};

static uint8_t *packet_templates;
static size_t packet_len;

static uint8_t *
template_for_flow(uint64_t flow_id)
{
	return packet_templates + (flow_id % num_flows) * packet_len;
}

static void
build_packet_template(uint8_t *dst, uint32_t flow_id)
{
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;
	uint8_t *payload;

	memset(dst, 0, packet_len);

	eth_hdr = (struct rte_ether_hdr *)dst;
	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(&static_server_eth, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	ipv4_hdr = (struct rte_ipv4_hdr *)(dst + RTE_ETHER_HDR_LEN);
	ipv4_hdr->version_ihl = 0x45;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length =
		rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				 sizeof(struct rte_udp_hdr) + payload_len);
	ipv4_hdr->packet_id = 0;
	ipv4_hdr->fragment_offset = 0;
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(my_ip);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(server_ip);

	udp_hdr = (struct rte_udp_hdr *)(dst + RTE_ETHER_HDR_LEN +
					 sizeof(struct rte_ipv4_hdr));
	udp_hdr->src_port = rte_cpu_to_be_16(flow_src_port(flow_id));
	udp_hdr->dst_port = rte_cpu_to_be_16(server_port);
	udp_hdr->dgram_len =
		rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_len);
	payload = (uint8_t *)(udp_hdr + 1);
	memset(payload, 0xAB, payload_len);

	ipv4_hdr->hdr_checksum = 0;
	ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
	udp_hdr->dgram_cksum = 0;
	udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
}

static void
prepare_templates(void)
{
	packet_len = RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr) +
		     sizeof(struct rte_udp_hdr) + payload_len;

	packet_templates = rte_zmalloc("fast_client_packet_templates",
				       packet_len * num_flows,
				       RTE_CACHE_LINE_SIZE);
	if (packet_templates == NULL) {
		rte_exit(EXIT_FAILURE,
			 "Cannot allocate packet templates for %u flows\n",
			 num_flows);
	}

	for (uint32_t i = 0; i < num_flows; i++) {
		build_packet_template(packet_templates + i * packet_len, i);
	}

	printf("Fast client templates prepared: flows=%u packet_len=%zu "
	       "tx_bursts_per_rx=%u\n",
	       num_flows,
	       packet_len,
	       FAST_TX_BURSTS_PER_RX);
}

static inline void
prepare_mbuf_from_template(struct rte_mbuf *mbuf, uint64_t flow_id)
{
	void *dst;

	rte_pktmbuf_reset(mbuf);
	dst = rte_pktmbuf_append(mbuf, packet_len);
	if (unlikely(dst == NULL)) {
		rte_exit(EXIT_FAILURE, "Cannot append %zu bytes to mbuf\n",
			 packet_len);
	}

	rte_memcpy(dst, template_for_flow(flow_id), packet_len);
	mbuf->l2_len = RTE_ETHER_HDR_LEN;
	mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
	mbuf->ol_flags = 0;
}

static inline void
drain_rx(uint8_t port, uint8_t queue_id, struct fast_client_worker_ctx *ctx)
{
	struct rte_mbuf *bufs[BURST_SIZE];
	uint16_t nb_rx;

	do {
		nb_rx = rte_eth_rx_burst(port, queue_id, bufs, BURST_SIZE);
		ctx->completed += nb_rx;
		for (uint16_t i = 0; i < nb_rx; i++) {
			rte_pktmbuf_free(bufs[i]);
		}
	} while (nb_rx == BURST_SIZE);
}

static int
fast_client_worker(void *arg)
{
	struct fast_client_worker_ctx *ctx = arg;
	uint8_t port = dpdk_port;
	struct rte_mbuf *pkts[BURST_SIZE];
	uint64_t next_flow = ctx->initial_flow_id;

	while (rte_get_timer_cycles() < ctx->start_cycles) {
		rte_pause();
	}

	while (rte_get_timer_cycles() < ctx->end_cycles) {
		for (unsigned int burst = 0; burst < FAST_TX_BURSTS_PER_RX; burst++) {
			if (unlikely(rte_pktmbuf_alloc_bulk(tx_mbuf_pool, pkts,
							    BURST_SIZE) != 0)) {
				ctx->alloc_failures++;
				drain_rx(port, ctx->queue_id, ctx);
				rte_pause();
				continue;
			}

			for (uint16_t i = 0; i < BURST_SIZE; i++) {
				prepare_mbuf_from_template(pkts[i], next_flow);
				next_flow += num_queues;
			}

			uint16_t nb_tx = rte_eth_tx_burst(port, ctx->queue_id,
							  pkts, BURST_SIZE);
			ctx->tx_sent += nb_tx;
			ctx->tx_drops += BURST_SIZE - nb_tx;

			if (unlikely(nb_tx < BURST_SIZE)) {
				for (uint16_t i = nb_tx; i < BURST_SIZE; i++) {
					rte_pktmbuf_free(pkts[i]);
				}
			}
		}

		drain_rx(port, ctx->queue_id, ctx);
	}

	drain_rx(port, ctx->queue_id, ctx);
	printf("Fast client worker queue=%u lcore=%u tx_sent=%" PRIu64
	       " tx_drops=%" PRIu64 " completed=%" PRIu64
	       " alloc_failures=%" PRIu64 "\n",
	       ctx->queue_id,
	       ctx->lcore_id,
	       ctx->tx_sent,
	       ctx->tx_drops,
	       ctx->completed,
	       ctx->alloc_failures);
	return 0;
}

static void
run_fast_client(uint8_t port)
{
	struct fast_client_worker_ctx *worker_ctxs;
	uint64_t start_cycles;
	uint64_t end_cycles;
	uint64_t total_completed = 0;
	uint64_t total_tx_sent = 0;
	uint64_t total_tx_drops = 0;
	uint64_t total_alloc_failures = 0;
	unsigned int next_queue = 0;
	unsigned int used_workers = 0;
	unsigned int launched_lcores[MAX_CORES];
	unsigned int launched_count = 0;
	unsigned int lcore_id;

	if (rte_lcore_count() < num_queues) {
		rte_exit(EXIT_FAILURE,
			 "Need at least %u lcores for %u fast client workers\n",
			 num_queues,
			 num_queues);
	}

	worker_ctxs = calloc(num_queues, sizeof(*worker_ctxs));
	if (worker_ctxs == NULL) {
		rte_exit(EXIT_FAILURE,
			 "Cannot allocate fast client worker contexts for %u queues\n",
			 num_queues);
	}

	prepare_templates();
	start_cycles = rte_get_timer_cycles() + rte_get_timer_hz() / 20;
	end_cycles = start_cycles + (uint64_t)seconds * rte_get_timer_hz();

	worker_ctxs[next_queue].queue_id = next_queue;
	worker_ctxs[next_queue].lcore_id = rte_lcore_id();
	worker_ctxs[next_queue].start_cycles = start_cycles;
	worker_ctxs[next_queue].end_cycles = end_cycles;
	worker_ctxs[next_queue].initial_flow_id = next_queue;
	used_workers++;
	next_queue++;

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (next_queue >= num_queues) {
			break;
		}

		worker_ctxs[next_queue].queue_id = next_queue;
		worker_ctxs[next_queue].lcore_id = lcore_id;
		worker_ctxs[next_queue].start_cycles = start_cycles;
		worker_ctxs[next_queue].end_cycles = end_cycles;
		worker_ctxs[next_queue].initial_flow_id = next_queue;
		if (rte_eal_remote_launch(fast_client_worker,
					  &worker_ctxs[next_queue],
					  lcore_id) != 0) {
			rte_exit(EXIT_FAILURE,
				 "Cannot launch fast client worker for queue %u on lcore %u\n",
				 next_queue,
				 lcore_id);
		}
		launched_lcores[launched_count++] = lcore_id;
		used_workers++;
		next_queue++;
	}

	if (used_workers < num_queues) {
		rte_exit(EXIT_FAILURE,
			 "Only mapped %u fast client workers for %u queues\n",
			 used_workers,
			 num_queues);
	}

	printf("\nFast DPDK Echo throughput test start now with %u workers\n",
	       num_queues);
	fast_client_worker(&worker_ctxs[0]);

	for (unsigned int i = 0; i < launched_count; i++) {
		if (rte_eal_wait_lcore(launched_lcores[i]) < 0) {
			rte_exit(EXIT_FAILURE,
				 "Fast client worker on lcore %u exited with error\n",
				 launched_lcores[i]);
		}
	}

	for (unsigned int i = 0; i < num_queues; i++) {
		total_completed += worker_ctxs[i].completed;
		total_tx_sent += worker_ctxs[i].tx_sent;
		total_tx_drops += worker_ctxs[i].tx_drops;
		total_alloc_failures += worker_ctxs[i].alloc_failures;
	}

	printf("Fast DPDK Echo client ran for %d seconds\n", seconds);
	printf("Fast DPDK Echo client tx_sent=%" PRIu64
	       " tx_drops=%" PRIu64 " completed=%" PRIu64
	       " alloc_failures=%" PRIu64 "\n",
	       total_tx_sent,
	       total_tx_drops,
	       total_completed,
	       total_alloc_failures);
	printf("Fast DPDK Echo client request-per-seconds: %f\n",
	       (float)total_completed / (float)seconds);
	printf("Fast DPDK Echo client throughput (Gbps): %f\n",
	       ((float)total_completed / (float)seconds) * packet_len * 8 / 1e9);

	rte_free(packet_templates);
	free(worker_ctxs);
	(void)port;
}

static int
parse_fast_client_args(int argc, char *argv[])
{
	long packet_size;
	long flow_count;
	long worker_count;
	size_t header_len;

	if (argc != 3 && argc != 4 && argc != 5) {
		printf("argument number incorrect: %d\n", argc);
		printf("usage: sudo ./packet_gen_client_fast <PACKET_SIZE> "
		       "<SERVER_MAC> [NUM_FLOWS] [NUM_WORKERS]\n");
		printf("example: sudo ./packet_gen_client_fast 64 "
		       "ec:b1:d7:85:5a:93 1024 8\n");
		return -EINVAL;
	}

	packet_size = strtol(argv[1], NULL, 10);
	header_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
		     sizeof(struct rte_udp_hdr);
	if (packet_size <= 0 || (size_t)packet_size < header_len) {
		printf("packet size must be at least %zu bytes\n", header_len);
		return -EINVAL;
	}

	payload_len = (size_t)packet_size - header_len;
	my_ip = DEFAULT_CLIENT_IP;
	server_ip = DEFAULT_SERVER_IP;
	num_queues = CLIENT_NUM_QUEUES;
	worker_count = CLIENT_NUM_QUEUES;

	if (argc >= 4) {
		flow_count = strtol(argv[3], NULL, 10);
		if (flow_count <= 0 ||
		    flow_count > (long)(UINT16_MAX - client_port)) {
			printf("num_flows must be in range [1, %u]\n",
			       (unsigned)(UINT16_MAX - client_port));
			return -EINVAL;
		}
		num_flows = (unsigned int)flow_count;
	} else {
		num_flows = DEFAULT_NUM_FLOWS;
	}

	if (argc == 5) {
		worker_count = strtol(argv[4], NULL, 10);
		if (worker_count <= 0 || worker_count > MAX_CORES) {
			printf("num_workers must be in range [1, %d]\n", MAX_CORES);
			return -EINVAL;
		}
		num_queues = (unsigned int)worker_count;
	}

	if (rte_ether_unformat_addr(argv[2], &static_server_eth) != 0) {
		printf("invalid server MAC address: %s\n", argv[2]);
		return -EINVAL;
	}

	printf("Fast client flows: %u (UDP src ports %u-%u)\n",
	       num_flows,
	       client_port,
	       client_port + num_flows - 1);
	printf("Fast client workers/queues: %u\n", num_queues);
	return 0;
}

int
main(int argc, char *argv[])
{
	int args_parsed;
	int res;

	args_parsed = dpdk_init(argc, argv);

	argc -= args_parsed;
	argv += args_parsed;
	res = parse_fast_client_args(argc, argv);
	if (res < 0) {
		return 0;
	}

	if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0) {
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu8 "\n",
			 dpdk_port);
	}

	run_fast_client(dpdk_port);

	return 0;
}

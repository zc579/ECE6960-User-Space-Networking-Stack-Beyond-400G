#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
#include <rte_udp.h>
#include "dpdk.h"

struct client_worker_ctx {
	uint8_t queue_id;
	unsigned int lcore_id;
	uint64_t start_cycles;
	uint64_t end_cycles;
	uint64_t initial_flow_id;
	uint64_t reqs;
	uint64_t tx_sent;
	uint64_t tx_drops;
};

static void client_latency_test(uint8_t port)
{
	uint64_t start_time, end_time;
	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	struct rte_udp_hdr *udp_hdr;
	uint32_t nb_tx, nb_rx, i;
	uint64_t correct_echos = 0, incorrect_echos = 0;
	uint64_t time_received;
	uint16_t expected_client_port;
	printf("\nDPDK Echo latency test start now!\n");
	/* Verify that we have enough space for all the datapoints, assuming
	   an RTT of at least 4 us */
	uint32_t samples = seconds / ((float) 4.0 / (1000*1000));
	if (samples > MAX_SAMPLES)
		rte_exit(EXIT_FAILURE, "Too many samples: %d\n", samples);

	/* run for specified amount of time */
	start_time = rte_get_timer_cycles();
	while (rte_get_timer_cycles() <
			start_time + seconds * rte_get_timer_hz()) {
		buf = rte_pktmbuf_alloc(tx_mbuf_pool);
		if (buf == NULL)
			printf("error allocating tx mbuf\n");

		craft_packet(buf, correct_echos);
		expected_client_port = rte_cpu_to_be_16(flow_src_port(correct_echos));
		
		/* send packet */
		snd_times[correct_echos] = rte_get_timer_cycles();
		nb_tx = rte_eth_tx_burst(port, 0, &buf, 1);

		if (unlikely(nb_tx != 1)) {
			printf("error: could not send packet\n");
		}

		nb_rx = 0;
		int total_received = 0;
		while (rte_get_timer_cycles() <
		       start_time + seconds * rte_get_timer_hz()) {
			nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
			time_received = rte_get_timer_cycles();
			if (nb_rx == 0)
				continue;

			// printf("got a packet back!");
			for (i = 0; i < nb_rx; i++) {
				buf = bufs[i];
				struct rte_ether_hdr *eth_hdr;

				if (!check_eth_hdr(buf))
					goto no_match;

				if (!check_ip_hdr(buf))
					goto no_match;

				eth_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
				if (!rte_is_same_ether_addr(&eth_hdr->src_addr,
							    &static_server_eth))
					goto no_match;

				udp_hdr = rte_pktmbuf_mtod_offset(
					buf,
					struct rte_udp_hdr *,
					RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
				if (udp_hdr->src_port != rte_cpu_to_be_16(server_port) ||
				    udp_hdr->dst_port != expected_client_port)
					goto no_match;

				/* packet matches */
				rte_pktmbuf_free(buf);
				rcv_times[correct_echos++] = time_received;
				goto next_batch;

			no_match:
				incorrect_echos++;
				/* packet isn't what we're looking for, free it and rx again */
				rte_pktmbuf_free(buf);
			
				goto next_batch;
			}
		}
		
	next_batch:
		total_received ++;
	}
	end_time = rte_get_timer_cycles();

	/* add up total cycles across all RTTs, skip first and last 10% */
	uint64_t total_cycles = 0;
	uint64_t included_samples = 0;
	for (i = correct_echos * 0.1; i < correct_echos * 0.9; i++) {
		total_cycles += rcv_times[i] - snd_times[i];
		included_samples++;
	}

	printf("DPDK Echo ran for %f seconds, receive %"PRIu64" correct echos, receive %"PRIu64" error echos \n",
			(float) (end_time - start_time) / rte_get_timer_hz(), correct_echos, incorrect_echos);
	if (included_samples > 0)
	  printf("mean latency (us): %f\n", (float) total_cycles *
		 1000 * 1000 / (included_samples * rte_get_timer_hz()));
}

static void client_throughput_test(uint8_t port) {
    uint64_t start_time, end_time;
    struct rte_mbuf *bufs[BURST_SIZE];
    struct rte_mbuf *pkts[BURST_SIZE];
    uint32_t nb_tx, nb_rx, i, created_pkts;
    uint64_t reqs = 0;
    uint64_t next_flow = 0;

	printf("\nDPDK Echo throughput test start now!\n");
    /* run for specified amount of time */
    start_time = rte_get_timer_cycles();
    while (rte_get_timer_cycles() <
           start_time + seconds * rte_get_timer_hz()) {

        /* 1. Prepare a burst of packets, varying UDP source ports per flow. */
        created_pkts = 0;
        for (i = 0; i < BURST_SIZE; i++) {
            pkts[i] = rte_pktmbuf_alloc(tx_mbuf_pool);
            if (unlikely(pkts[i] == NULL)) {
                break;
            }
            craft_packet(pkts[i], next_flow++);
            created_pkts++;
        }
        
        /* 2. Send the burst of created packets. */
        if (likely(created_pkts > 0)) {
             nb_tx = rte_eth_tx_burst(port, 0, pkts, created_pkts);

            /* 3. Free any packets that were cloned but not sent. */
            if (unlikely(nb_tx < created_pkts)) {
                for (i = nb_tx; i < created_pkts; i++) {
                    rte_pktmbuf_free(pkts[i]);
                }
            }
        }
        
        /* 4. Try to receive a burst of packets. */
        nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);

        if (nb_rx == 0)
            continue;
        
        reqs += nb_rx;
        
        /* 5. Free the received packets. */
        for (i = 0; i < nb_rx; i++) {
            rte_pktmbuf_free(bufs[i]);
        }
    }
    end_time = rte_get_timer_cycles();

    float rps = (float)(reqs * rte_get_timer_hz()) / (end_time - start_time);
	printf("DPDK Echo runs %f seconds, completed %" PRIu64 " echos\n",
           (float)(end_time - start_time) / rte_get_timer_hz(), reqs);
	printf("DPDK Echo client request-per-seconds: %f\n",
           rps);
	printf("DPDK Echo client throughput (Gbps): %f\n",
           rps * (payload_len + sizeof(struct rte_ether_hdr) +
                  sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr)) *
                     8 / 1e9);
}

static int
client_throughput_worker(void *arg)
{
	struct client_worker_ctx *ctx = arg;
	uint8_t port = dpdk_port;
	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *pkts[BURST_SIZE];
	uint64_t next_flow = ctx->initial_flow_id;

	while (rte_get_timer_cycles() < ctx->start_cycles) {
		rte_pause();
	}

	while (rte_get_timer_cycles() < ctx->end_cycles) {
		uint32_t created_pkts = 0;

		for (uint32_t i = 0; i < BURST_SIZE; i++) {
			pkts[i] = rte_pktmbuf_alloc(tx_mbuf_pool);
			if (unlikely(pkts[i] == NULL)) {
				break;
			}
			craft_packet(pkts[i], next_flow);
			next_flow += num_queues;
			created_pkts++;
		}

		if (likely(created_pkts > 0)) {
			uint32_t nb_tx = rte_eth_tx_burst(port, ctx->queue_id, pkts,
							      created_pkts);
			ctx->tx_sent += nb_tx;
			ctx->tx_drops += created_pkts - nb_tx;

			if (unlikely(nb_tx < created_pkts)) {
				for (uint32_t i = nb_tx; i < created_pkts; i++) {
					rte_pktmbuf_free(pkts[i]);
				}
			}
		}

		{
			uint32_t nb_rx =
				rte_eth_rx_burst(port, ctx->queue_id, bufs, BURST_SIZE);
			ctx->reqs += nb_rx;
			for (uint32_t i = 0; i < nb_rx; i++) {
				rte_pktmbuf_free(bufs[i]);
			}
		}
	}

	printf("Client worker queue=%u lcore=%u tx_sent=%" PRIu64
	       " tx_drops=%" PRIu64 " completed=%" PRIu64 "\n",
	       ctx->queue_id,
	       ctx->lcore_id,
	       ctx->tx_sent,
	       ctx->tx_drops,
	       ctx->reqs);
	return 0;
}

static void
run_client_multiworker(uint8_t port)
{
	struct client_worker_ctx *worker_ctxs;
	uint64_t start_cycles;
	uint64_t end_cycles;
	uint64_t total_reqs = 0;
	uint64_t total_tx_sent = 0;
	uint64_t total_tx_drops = 0;
	unsigned int next_queue = 0;
	unsigned int used_workers = 0;
	unsigned int launched_lcores[MAX_CORES];
	unsigned int launched_count = 0;
	unsigned int lcore_id;

	if (rte_lcore_count() < num_queues) {
		rte_exit(EXIT_FAILURE,
			 "Need at least %u lcores for %u client workers\n",
			 num_queues,
			 num_queues);
	}

	worker_ctxs = calloc(num_queues, sizeof(*worker_ctxs));
	if (worker_ctxs == NULL) {
		rte_exit(EXIT_FAILURE,
			 "Cannot allocate client worker contexts for %u queues\n",
			 num_queues);
	}

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
		if (rte_eal_remote_launch(client_throughput_worker,
					  &worker_ctxs[next_queue],
					  lcore_id) != 0) {
			rte_exit(EXIT_FAILURE,
				 "Cannot launch client worker for queue %u on lcore %u\n",
				 next_queue,
				 lcore_id);
		}
		launched_lcores[launched_count++] = lcore_id;
		used_workers++;
		next_queue++;
	}

	if (used_workers < num_queues) {
		rte_exit(EXIT_FAILURE,
			 "Only mapped %u client workers for %u queues\n",
			 used_workers,
			 num_queues);
	}

	client_throughput_worker(&worker_ctxs[0]);

	for (unsigned int i = 0; i < launched_count; i++) {
		if (rte_eal_wait_lcore(launched_lcores[i]) < 0) {
			rte_exit(EXIT_FAILURE,
				 "Client worker on lcore %u exited with error\n",
				 launched_lcores[i]);
		}
	}

	for (unsigned int i = 0; i < num_queues; i++) {
		total_reqs += worker_ctxs[i].reqs;
		total_tx_sent += worker_ctxs[i].tx_sent;
		total_tx_drops += worker_ctxs[i].tx_drops;
	}

	printf("DPDK Echo multi-core client ran for %d seconds\n", seconds);
	printf("DPDK Echo multi-core client tx_sent=%" PRIu64
	       " tx_drops=%" PRIu64 " completed=%" PRIu64 "\n",
	       total_tx_sent,
	       total_tx_drops,
	       total_reqs);
	printf("DPDK Echo multi-core client request-per-seconds: %f\n",
	       (float)total_reqs / (float)seconds);
	printf("DPDK Echo multi-core client throughput (Gbps): %f\n",
	       ((float)total_reqs / (float)seconds) *
		       (payload_len + sizeof(struct rte_ether_hdr) +
			sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr)) * 8 /
		       1e9);
	free(worker_ctxs);
	(void)port;
}

/*
 * Run an echo client
 */
static void run_client(uint8_t port)
{
	char mac_buf[64];

	printf("\nCore %u running in client mode. [Ctrl+C to quit]\n",
			rte_lcore_id());

	rte_ether_format_addr(&mac_buf[0], 64, &static_server_eth);
	printf("Using static server MAC addr: %s\n", &mac_buf[0]);

	if (num_queues == 1) {
		client_latency_test(port);
		client_throughput_test(port);
		return;
	}

	printf("Multi-worker client mode enabled with %u queues/workers; "
	       "latency test is skipped and throughput workers run in parallel.\n",
	       num_queues);
	run_client_multiworker(port);
}

static int parse_echo_args(int argc, char *argv[])
{
	long packet_size;
	long flow_count;
	long worker_count;
	size_t header_len;

	if (argc != 3 && argc != 4 && argc != 5) {
        printf("argument number incorrect: %d\n", argc);
        printf("usage: sudo ./packet_gen_client <PACKET_SIZE> <SERVER_MAC> [NUM_FLOWS] [NUM_WORKERS]\n");
        printf("example: sudo ./packet_gen_client 64 ec:b1:d7:85:5a:93 128 2\n");
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

	if (argc == 4) {
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

	/* parse static server MAC addr from XX:XX:XX:XX:XX:XX */
	if (rte_ether_unformat_addr(argv[2], &static_server_eth) != 0) {
		printf("invalid server MAC address: %s\n", argv[2]);
		return -EINVAL;
	}
	printf("Client flows: %u (UDP src ports %u-%u)\n",
	       num_flows,
	       client_port,
	       client_port + num_flows - 1);
	printf("Client workers/queues: %u\n", num_queues);
	return 0;
}

/*
 * The main function, which does initialization and starts the client or server.
 */
int
main(int argc, char *argv[])
{
	int args_parsed, res;

	/* Initialize dpdk. */
	args_parsed = dpdk_init(argc, argv);

	/* initialize our arguments */
	argc -= args_parsed;
	argv += args_parsed;
	res = parse_echo_args(argc, argv);
	if (res < 0)
		return 0;

	/* initialize port */
	if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", dpdk_port);

	run_client(dpdk_port);


	return 0;
}

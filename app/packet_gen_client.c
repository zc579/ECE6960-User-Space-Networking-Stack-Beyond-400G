
Page
1
of 5
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
static void client_latency_test(uint8_t port)
{
uint64_t start_time, end_time;
struct rte_mbuf *bufs[BURST_SIZE];
struct rte_mbuf *buf;
struct rte_udp_hdr *rte_udp_hdr;
uint32_t nb_tx, nb_rx, i;
uint64_t correct_echos = 0, incorrect_echos = 0;
struct rte_ether_addr server_eth;
uint64_t time_received;
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
craft_packet(buf, port);
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
if (!check_eth_hdr(buf))
goto no_match;
/* this packet is IPv4, check IP header */
if (!check_ip_hdr(buf))
goto no_match;
/* check UDP header */
rte_udp_hdr = rte_pktmbuf_mtod_offset(buf, struct
rte_udp_hdr *,
RTE_ETHER_HDR_LEN + sizeof(struct
rte_ipv4_hdr));
if (rte_udp_hdr->src_port != rte_cpu_to_be_16(server_port)
||
rte_udp_hdr->dst_port != rte_cpu_to_be_16(client_port))
goto no_match;
/* packet matches */
rte_pktmbuf_free(buf);
rcv_times[correct_echos++] = time_received;
goto next_batch;
no_match:
incorrect_echos++;
/* packet isn't what we're looking for, free it and rx
again */
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
printf("DPDK Echo ran for %f seconds, receive %"PRIu64" correct echos,
receive %"PRIu64" error echos \n",
(float) (end_time - start_time) / rte_get_timer_hz(),
correct_echos, incorrect_echos);
if (included_samples > 0)
printf("mean latency (us): %f\n", (float) total_cycles *
1000 * 1000 / (included_samples * rte_get_timer_hz()));
}
static void client_throughput_test(uint8_t port) {
uint64_t start_time, end_time;
struct rte_mbuf *bufs[BURST_SIZE];
struct rte_mbuf *pkts[BURST_SIZE];
struct rte_mbuf *template_buf = rte_pktmbuf_alloc(tx_mbuf_pool);
uint32_t nb_tx, nb_rx, i, created_pkts;
uint64_t reqs = 0;
printf("\nDPDK Echo throughput test start now!\n");
// Check if the template packet was allocated successfully
if (unlikely(template_buf == NULL)) {
rte_exit(EXIT_FAILURE, "Failed to allocate template mbuf\n");
}
craft_packet(template_buf, port);
/* run for specified amount of time */
start_time = rte_get_timer_cycles();
while (rte_get_timer_cycles() <
start_time + seconds * rte_get_timer_hz()) {
/* 1. Prepare a burst of packets by cloning the template. */
created_pkts = 0;
for (i = 0; i < BURST_SIZE; i++) {
pkts[i] = rte_pktmbuf_clone(template_buf, tx_mbuf_pool);
// Stop creating if the memory pool runs out of buffers
if (unlikely(pkts[i] == NULL)) {
break;
}
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
/* 6. Clean up the original template packet. */
rte_pktmbuf_free(template_buf);
float rps = (float)(reqs * rte_get_timer_hz()) / (end_time - start_time);
printf("DPDK Echo runs %f seconds, completed %" PRIu64 " echos\n",
(float)(end_time - start_time) / rte_get_timer_hz(), reqs);
printf("DPDK Echo client request-per-seconds: %f\n",
rps);
printf("DPDK Echo client throughput (Gbps): %f\n",
rps * (payload_len + sizeof(struct rte_ether_hdr) + sizeof(struct
rte_ipv4_hdr) + sizeof(struct rte_udp_hdr)) * 8 / 1e9);
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
client_latency_test(port);
client_throughput_test(port);
}
static int parse_echo_args(int argc, char *argv[])
{
long tmp;
int next_arg;
if (argc < 3) {
printf("argument number incorrect: %d\n", argc);
printf("usage: sudo ./packet_gen_client <PACKET_SIZE> <CLIENT_IP>
<SERVER_IP> <SERVER_MAC>\n");
printf("example: sudo ./packet_gen_client 64 10.16.1.2 10.16.1.1
ec:b1:d7:85:5a:93\n");
return -EINVAL;
}
str_to_ip(argv[2], &my_ip);
payload_len = (atoi(argv[1])) - (sizeof(struct rte_ether_hdr) + sizeof(struct
rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
argc -= 3;
next_arg = 3;
str_to_ip(argv[next_arg++], &server_ip);
/* parse static server MAC addr from XX:XX:XX:XX:XX:XX */
rte_ether_unformat_addr(argv[next_arg++],
&static_server_eth);
printf("Client IP: %u.%u.%u.%u\n",
(my_ip >> 24) & 0xFF, (my_ip >> 16) & 0xFF,
(my_ip >> 8) & 0xFF, my_ip & 0xFF);
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

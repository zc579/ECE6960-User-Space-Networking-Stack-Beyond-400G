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

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define MAX_CORES 64
#define UDP_MAX_PAYLOAD 1472
#define MAX_SAMPLES (10*1000*1000)

#define FULL_MASK 0xFFFFFFFF
#define EMPTY_MASK 0x0
#define SERVER_NUM_QUEUES 2
#define CLIENT_NUM_QUEUES 1
#define DEFAULT_NUM_FLOWS 128
#define MAKE_IP_ADDR(a, b, c, d) \
	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
	 ((uint32_t)(c) << 8) | (uint32_t)(d))
#define DEFAULT_CLIENT_IP MAKE_IP_ADDR(10, 16, 1, 2)
#define DEFAULT_SERVER_IP MAKE_IP_ADDR(10, 16, 1, 1)

static uint64_t snd_times[MAX_SAMPLES];
static uint64_t rcv_times[MAX_SAMPLES];

/* parameters */
static int seconds = 5;
size_t payload_len = 22; /* total packet size of 64 bytes */
static unsigned int client_port = 50000;
static unsigned int server_port = 8001;
static unsigned int num_queues = CLIENT_NUM_QUEUES;
static unsigned int num_flows = DEFAULT_NUM_FLOWS;


/* offload checksum calculations */
static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_NONE,
        .offloads = 0,
    },
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE,
        .offloads = 0,
    },
};

static unsigned int dpdk_port = 0;
static uint8_t mode;
struct rte_mempool *rx_mbuf_pool;
struct rte_mempool *tx_mbuf_pool;
static struct rte_ether_addr my_eth;
static uint32_t my_ip;
static uint32_t server_ip;
struct rte_ether_addr zero_mac = {
		.addr_bytes = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
};
struct rte_ether_addr broadcast_mac = {
		.addr_bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};


/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_queues)
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

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0)
        return retval;

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
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    rxconf = dev_info.default_rxconf;
    txconf = dev_info.default_txconf;

    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port),
                                        &rxconf, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port),
                                        &txconf);
        if (retval < 0)
            return retval;
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

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
 * Validate this Ethernet header for our IPv4 echo packets.
 */
static bool check_eth_hdr(struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		/* packet not IPv4 */
		return false;

	return true;
}

/*
 * Return true if this IP packet is to us and contains a UDP payload.
 */
static bool check_ip_hdr(struct rte_mbuf *buf)
{
	struct rte_ipv4_hdr *ipv4_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,
			RTE_ETHER_HDR_LEN);
	if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(my_ip) ||
	    ipv4_hdr->next_proto_id != IPPROTO_UDP)
		return false;

	return true;
}

struct rte_ether_addr static_server_eth;

static inline uint16_t
flow_src_port(uint32_t flow_id)
{
	return (uint16_t)(client_port + (flow_id % num_flows));
}

static inline void
set_packet_flow(struct rte_mbuf *buf, uint32_t flow_id)
{
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,
			RTE_ETHER_HDR_LEN);
	udp_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_udp_hdr *,
			RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
	udp_hdr->src_port = rte_cpu_to_be_16(flow_src_port(flow_id));
	udp_hdr->dgram_cksum = 0;
	udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
}

static void craft_packet(struct rte_mbuf *buf, uint32_t flow_id)
{
	char *buf_ptr;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *udp_hdr;

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(buf, RTE_ETHER_HDR_LEN);
	eth_hdr = (struct rte_ether_hdr *) buf_ptr;

	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(&static_server_eth, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* IPv4 header */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_ipv4_hdr));
	ipv4_hdr = (struct rte_ipv4_hdr *)buf_ptr;
	ipv4_hdr->version_ihl = 0x45;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
			sizeof(struct rte_udp_hdr) + payload_len);
	ipv4_hdr->packet_id = 0;
	ipv4_hdr->fragment_offset = 0;
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->hdr_checksum = 0;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(my_ip);
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(server_ip);

	/* UDP header + payload */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_udp_hdr) + payload_len);
	udp_hdr = (struct rte_udp_hdr *)buf_ptr;
	udp_hdr->src_port = rte_cpu_to_be_16(client_port);
	udp_hdr->dst_port = rte_cpu_to_be_16(server_port);
	udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_len);
	udp_hdr->dgram_cksum = 0;
	memset(buf_ptr + sizeof(struct rte_udp_hdr), 0xAB, payload_len);

	buf->l2_len = RTE_ETHER_HDR_LEN;
	buf->l3_len = sizeof(struct rte_ipv4_hdr);
	buf->ol_flags = 0;
	ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
	set_packet_flow(buf, flow_id);
}

/*
 * Initialize dpdk.
 */
static int dpdk_init(int argc, char *argv[])
{
	int args_parsed;
	int ret;

	args_parsed = rte_eal_init(argc, argv);
	if (args_parsed < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	rx_mbuf_pool = rte_pktmbuf_pool_create("RX_POOL", NUM_MBUFS * 2,
			MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (rx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create RX mbuf pool\n");

	tx_mbuf_pool = rte_pktmbuf_pool_create("TX_POOL", NUM_MBUFS * 2,
			MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (tx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create TX mbuf pool\n");

	ret = rte_eth_dev_count_avail();
	if (ret == 0)
		rte_exit(EXIT_FAILURE, "No available Ethernet ports\n");

	return args_parsed;
}

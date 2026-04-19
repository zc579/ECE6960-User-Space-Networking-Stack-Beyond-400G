#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

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
#define ECHO_ETHER_TYPE 0x88B5

static uint64_t snd_times[MAX_SAMPLES];
static uint64_t rcv_times[MAX_SAMPLES];

/* parameters */
static int seconds = 5;
size_t payload_len = 46; /* total Ethernet frame size of 64 bytes */
static unsigned int num_queues = 1;


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
 * Validate this Ethernet header for our custom L2 echo protocol.
 */
static bool check_eth_hdr(struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(ECHO_ETHER_TYPE))
		/* packet not our custom echo frame */
		return false;

	return true;
}

struct rte_ether_addr static_server_eth;

static void craft_packet(struct rte_mbuf *buf)
{
	char *buf_ptr;
	struct rte_ether_hdr *eth_hdr;

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(buf, RTE_ETHER_HDR_LEN);
	eth_hdr = (struct rte_ether_hdr *) buf_ptr;

	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(&static_server_eth, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(ECHO_ETHER_TYPE);

	/* payload */
	buf_ptr = rte_pktmbuf_append(buf, payload_len);
	memset(buf_ptr, 0xAB, payload_len);

	buf->l2_len = RTE_ETHER_HDR_LEN;
	buf->l3_len = 0;
	buf->ol_flags = 0;
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

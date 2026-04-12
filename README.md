# ECE6960 Final project

This repository currently tracks the warmup DPDK echo-server assignment and related code for the ECE6960 project.

## Files
- `app/echo_server.c`: DPDK echo server implementation
- `app/packet_gen_client.c`: DPDK packet generator for RTT and throughput measurements
- `app/dpdk.h`: shared DPDK helper definitions used by both programs
- `Makefile`: root-level build entry that compiles both apps

## Quick use on CloudLab
On both nodes:

```bash
git clone git@github.com:zc579/ECE6960-User-Space-Networking-Stack-Beyond-400G.git
cd ECE6960-User-Space-Networking-Stack-Beyond-400G
make
```

If `make` cannot find `libdpdk`, make sure DPDK is installed and that `pkg-config` can locate `libdpdk.pc`.

On node 0:

```bash
sudo ./echo_server 10.16.1.1
```

On node 1:

```bash
sudo ./packet_gen_client 64 10.16.1.2 10.16.1.1 ec:b1:d7:85:2a:93
```

## Echo server profiling
The echo server now prints stage-level profiling once per second. The datapath is split into:

- `RX`: receiving packets with `rte_eth_rx_burst`
- `Parse`: locating Ethernet, IPv4, and UDP headers inside each mbuf
- `Rewrite`: swapping Ethernet source/destination MACs, IPv4 source/destination addresses, and UDP source/destination ports
- `Checksum`: recomputing IPv4 and UDP checksums after rewriting headers
- `TX`: sending the packet back with `rte_eth_tx_burst`

Example output:

```text
[profile] rx_pkts=... tx_pkts=... tx_drops=... rx_mpps=... tx_mpps=... avg_burst=... rx_cycles/burst=... rx_cycles/pkt=... parse_cycles/pkt=... rewrite_cycles/pkt=... checksum_cycles/pkt=... tx_cycles/pkt=... cpu_ghz=...
```

Field meanings:

- `rx_pkts` / `tx_pkts`: packets received and echoed during the last reporting interval
- `tx_drops`: packets that could not be transmitted and were freed
- `rx_mpps` / `tx_mpps`: receive/transmit rate in million packets per second
- `avg_burst`: average number of packets returned by each `rte_eth_rx_burst` call
- `rx_cycles/burst`: average CPU cycles per RX burst call
- `rx_cycles/pkt`: RX cost amortized per received packet
- `parse_cycles/pkt`: per-packet header parsing cost
- `rewrite_cycles/pkt`: per-packet header swap cost
- `checksum_cycles/pkt`: per-packet checksum recomputation cost
- `tx_cycles/pkt`: per-packet TX cost

`BURST_SIZE` is defined in `app/dpdk.h`. It is the maximum number of packets that one RX burst call tries to receive. If `avg_burst` is close to 1, batching is not being used effectively; if it is closer to `BURST_SIZE`, the fixed RX cost is better amortized across packets.

Use these numbers to identify the first bottleneck. For example, high `tx_cycles/pkt` suggests the current per-packet TX path should be optimized with batched transmit. High `checksum_cycles/pkt` suggests checksum recomputation or checksum offload should be investigated.

## Notes
- Source files now live under `app/`, and the shared header is `app/dpdk.h`.
- The current `echo_server` and `packet_gen_client` follow the starter-code style that uses helpers from `dpdk.h`.
- The experimental NIC selection still depends on the logic inside `app/dpdk.h`, so confirm that `dpdk_port` refers to the experimental interface before running on CloudLab.

## Plotting single-core stage share
You can save the server output and use the plotting script in `analysis/` to generate a bar chart for the stage breakdown.

Example:

```bash
mkdir -p results/raw
sudo ./echo_server 10.16.1.1 | tee results/raw/echo_single_core.log
python3 analysis/plot_single_core_stage_breakdown.py results/raw/echo_single_core.log
```

By default, the script selects the `[profile]` sample with the highest `rx_mpps` and writes:

```text
results/processed/single_core_stage_breakdown.png
```

You can also choose the last sample explicitly:

```bash
python3 analysis/plot_single_core_stage_breakdown.py \
  results/raw/echo_single_core.log \
  --mode last
```

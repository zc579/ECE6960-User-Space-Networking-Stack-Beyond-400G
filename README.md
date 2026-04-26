# ECE6960 Final Project

This repo contains a DPDK IPv4/UDP echo server and a packet generator client for latency, throughput, RSS, and multi-core scaling experiments.

## Files
- `app/echo_server.c`: echo server with per-worker profiling
- `app/packet_gen_client.c`: latency test plus single-core or multi-core throughput generator
- `app/dpdk.h`: shared DPDK config, packet format, RSS setup, and helpers
- `analysis/plot_single_core_stage_breakdown.py`: plot one server `[profile]` sample as a stage breakdown

## Prerequisites
- DPDK must be installed and visible to `pkg-config`
- the target NIC port must be bound to a DPDK-compatible driver such as `vfio-pci`
- the test port selected by `dpdk_port` in `app/dpdk.h` must match your experiment NIC

Build:

```bash
make
```

## Packet Format
The programs exchange fixed-format Ethernet + IPv4 + UDP echo packets.

Built-in addresses:
- server IP: `10.16.1.1`
- client IP: `10.16.1.2`
- server UDP port: `8001`
- client UDP source ports: `50000 ... 50000 + NUM_FLOWS - 1`

Notes:
- these IPs are application-defined packet fields; they do not need to match a Linux interface IP
- the server enables IPv4/UDP RSS automatically when `NUM_WORKERS > 1`
- to benefit from RSS, the client should send multiple flows, for example `NUM_FLOWS=128`

## Usage
Server:

```bash
sudo ./echo_server -l <lcore_list> -- <NUM_WORKERS>
```

Examples:

```bash
sudo ./echo_server -l 0 -- 1
sudo ./echo_server -l 0,1 -- 2
sudo ./echo_server -l 0,1,2,3 -- 4
```

Client:

```bash
sudo ./packet_gen_client -l <lcore_list> -- <PACKET_SIZE> <SERVER_MAC> [NUM_FLOWS] [NUM_WORKERS]
```

Examples:

```bash
sudo ./packet_gen_client -l 0 -- 64 <SERVER_MAC> 128 1
sudo ./packet_gen_client -l 0,1 -- 64 <SERVER_MAC> 128 2
sudo ./packet_gen_client -l 0,1,2,3 -- 64 <SERVER_MAC> 128 4
```

Argument behavior:
- `NUM_WORKERS` is the number of RX/TX queues and worker lcores used by the program
- server mode requires at least `NUM_WORKERS` lcores in the EAL `-l` list
- client mode also requires at least `NUM_WORKERS` lcores
- `PACKET_SIZE` is the full Ethernet frame size used by the app and must be at least `42` bytes
- if `NUM_WORKERS == 1`, the client runs a latency test followed by a throughput test
- if `NUM_WORKERS > 1`, the client skips latency and runs parallel throughput workers only

## Quick Start
On the server node:

```bash
make
sudo ./echo_server -l 0,1 -- 2
```

On the client node:

```bash
make
sudo ./packet_gen_client -l 0,1 -- 64 30:3e:a7:1e:e1:20 128 2
```

What to expect:
- the server prints one `[profile]` line per worker per second
- the client prints either latency + throughput results for one worker, or per-worker throughput plus an aggregate total for multi-worker runs
- if only one server queue receives traffic in a multi-core run, increase `NUM_FLOWS` and confirm RSS is supported on the NIC

## Server Profiling
Each server worker reports one `[profile]` line per second with `queue_id=...` and `lcore=...`.

The measured stages are:
- `RX`: one `rte_eth_rx_burst()` call
- `Parse`: locating Ethernet, IPv4, and UDP headers for the whole batch
- `Rewrite`: swapping MAC, IP, and UDP endpoint fields for the whole batch
- `Checksum`: recomputing IPv4 and UDP checksums for the whole batch
- `TX`: one `rte_eth_tx_burst()` call

The profile line also includes explicit loop accounting:
- `poll_count`: total RX polls
- `empty_poll_count`: polls with `nb_rx == 0`
- `nonempty_poll_count`: polls with `nb_rx > 0`
- `work_batches`: processed non-empty batches
- `total_cycles/poll`: average cycles per RX loop iteration
- `*_cycles/nonempty_batch`: stage cost normalized by non-empty polls
- `*_cycles/pkt`: stage cost normalized by packets
- `empty_poll_cycles/empty_poll`: true empty-poll cost
- `nonempty_poll_cycles/nonempty_poll`: true non-empty-poll loop cost
- `other_cycles/nonempty_batch`: derived loop/control overhead not explained by RX, parse, rewrite, checksum, and TX

Example:

```text
[profile] queue_id=0 lcore=0 rx_pkts=... tx_pkts=... tx_drops=... poll_count=... empty_poll_count=... nonempty_poll_count=... work_batches=... rx_mpps=... tx_mpps=... avg_burst=... total_cycles/poll=... total_cycles/nonempty_batch=... total_cycles/pkt=... checksum_cycles/nonempty_batch=... checksum_cycles/pkt=... other_cycles/nonempty_batch=... other_cycles/pkt=... cpu_ghz=...
```

For a multi-core run, total server throughput is the sum of all worker lines.

## Plotting
Save the server log and run the plotting script:

```bash
mkdir -p results/raw
sudo ./echo_server -l 0 -- 1 | tee results/raw/echo.log
python3 analysis/plot_single_core_stage_breakdown.py results/raw/echo.log
```

Optional flags:

```bash
python3 analysis/plot_single_core_stage_breakdown.py results/raw/echo.log --mode last
python3 analysis/plot_single_core_stage_breakdown.py results/raw/echo.log --output results/processed/stage_breakdown.png --title "Single-Core Echo Breakdown"
```

Notes:
- the script reads `[profile]` lines from the server log
- by default it picks the sample with the highest `rx_mpps`
- on a multi-core log it still plots one worker sample at a time, not an aggregate

## Minimal Environment Checklist
If the binaries do not run yet, verify these first:

```bash
pkg-config --modversion libdpdk
sudo /usr/local/bin/dpdk-devbind.py --status
```

The expected state is:
- `pkg-config` finds `libdpdk`
- your experiment NIC appears under a DPDK-compatible driver

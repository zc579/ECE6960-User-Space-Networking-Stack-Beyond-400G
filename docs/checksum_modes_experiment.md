# Checksum Modes Experiment

## Goal

The server profile shows that IPv4/UDP checksum recomputation is a large
per-packet cost. This experiment compares three practical checksum strategies
for the echo server and asks whether recomputation is actually necessary for
this symmetric IPv4/UDP echo workload.

## Modes

`software` is the baseline. The server swaps Ethernet, IPv4, and UDP endpoints,
then recomputes the IPv4 header checksum and UDP checksum in software.

`offload` requests NIC TX offload for IPv4 and UDP checksums. The server still
prepares each packet by setting mbuf offload metadata and initializing checksum
fields according to DPDK TX checksum offload semantics.

`preserve` swaps the same symmetric fields but leaves the original IPv4 and UDP
checksum fields unchanged. This may be correct for echo because one's-complement
sums are invariant under swapping source and destination fields when packet
lengths and payload are unchanged.

`none` is a debugging mode that clears checksums. It is useful as a negative
control, but it should not be used as the main experimental result.

## Why Preserve May Be Correct

IPv4 checksum covers the IPv4 header. Swapping `src_ip` and `dst_ip` does not
change the one's-complement sum of the header words.

UDP checksum covers the pseudo-header, UDP header, and payload. Swapping
`src_ip` with `dst_ip` and `src_port` with `dst_port` preserves the sum as long
as length fields and payload remain unchanged.

This reasoning is specific to this symmetric echo transformation. A network
function that changes TTL, payload, length, addresses asymmetrically, ports
asymmetrically, or encapsulation headers still needs checksum update,
recomputation, or offload.

## Running The Experiment

Build:

```bash
make
```

Create result directories:

```bash
mkdir -p results/raw/checksum_modes/software
mkdir -p results/raw/checksum_modes/offload
mkdir -p results/raw/checksum_modes/preserve
mkdir -p results/processed
```

Run each server command, then run the matching `packet_gen_client` command on
the client node. Keep packet size, flows, core placement, and test duration the
same for all modes.

1 core:

```bash
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0 -- --checksum-mode=software 1 | tee results/raw/checksum_modes/software/server_1c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0 -- --checksum-mode=offload 1 | tee results/raw/checksum_modes/offload/server_1c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0 -- --checksum-mode=preserve 1 | tee results/raw/checksum_modes/preserve/server_1c.log
```

2 cores:

```bash
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1 -- --checksum-mode=software 2 | tee results/raw/checksum_modes/software/server_2c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1 -- --checksum-mode=offload 2 | tee results/raw/checksum_modes/offload/server_2c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1 -- --checksum-mode=preserve 2 | tee results/raw/checksum_modes/preserve/server_2c.log
```

4 cores:

```bash
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3 -- --checksum-mode=software 4 | tee results/raw/checksum_modes/software/server_4c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3 -- --checksum-mode=offload 4 | tee results/raw/checksum_modes/offload/server_4c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3 -- --checksum-mode=preserve 4 | tee results/raw/checksum_modes/preserve/server_4c.log
```

6 cores:

```bash
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3,4,5 -- --checksum-mode=software 6 | tee results/raw/checksum_modes/software/server_6c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3,4,5 -- --checksum-mode=offload 6 | tee results/raw/checksum_modes/offload/server_6c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3,4,5 -- --checksum-mode=preserve 6 | tee results/raw/checksum_modes/preserve/server_6c.log
```

8 cores:

```bash
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3,4,5,6,7 -- --checksum-mode=software 8 | tee results/raw/checksum_modes/software/server_8c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3,4,5,6,7 -- --checksum-mode=offload 8 | tee results/raw/checksum_modes/offload/server_8c.log
timeout -s INT 15s sudo stdbuf -oL -eL ./echo_server_checksum_exp -l 0,1,2,3,4,5,6,7 -- --checksum-mode=preserve 8 | tee results/raw/checksum_modes/preserve/server_8c.log
```

Generate the comparison:

```bash
python3 analysis/plot_checksum_modes_comparison.py
```

Outputs:

```text
results/processed/checksum_modes_stage_cycles.png
results/processed/checksum_modes_summary.csv
```

## Metrics To Inspect

First compare `checksum_cycles_per_pkt`. Preserve supports the hypothesis if it
is near zero and lower than both software recomputation and offload preparation.

Then compare `total_cycles_per_pkt`, `rx_mpps`, and `tx_mpps`. The strongest
result would be preserve reducing total cycles and improving or stabilizing
throughput, especially at 6 and 8 cores.

Finally inspect `rx_cycles_per_pkt`, `tx_cycles_per_pkt`, and
`other_cycles_per_pkt`. If checksum cost drops but these grow, the bottleneck has
moved to RX/TX queue pressure, cache effects, NUMA placement, or other loop
overhead.

## Correctness Validation

Performance numbers alone are not enough. Validate that:

- `packet_gen_client` still receives valid echo replies.
- Server `rx_pkts` and `tx_pkts` match as closely as possible.
- `tx_drops` are zero or explainable near saturation.
- RSS still spreads traffic across queues in multi-core runs.

To inspect checksums, capture returned packets on the receiving side or on a
mirror/tap after transmission:

```bash
sudo tcpdump -i <client-interface> -vvv -nn udp and port 8001
```

Wireshark can also be used on the capture file to verify IPv4 and UDP checksum
validity. Prefer receiving-side captures because captures taken before a NIC
completes TX offload can show misleading checksum values.

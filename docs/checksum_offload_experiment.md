# Checksum Offload Experiment

## Goal

The 100G scaling logs show that checksum recomputation is one of the largest
per-packet server stages, and that the 8-core run becomes more expensive per
packet instead of scaling further. This experiment tests whether server-side
software checksum recomputation explains a meaningful part of that bottleneck.

## Server Modes

`echo_server_before_checksum_offload` is the unchanged software-checksum
baseline copied from the original server implementation.

`echo_server_checksum_offload` defaults to the same software checksum behavior:

```bash
sudo ./echo_server_checksum_offload -l 0,1,2,3 -- --checksum-mode=software 4
```

In offload mode, the server requests IPv4 and UDP TX checksum offloads from the
NIC, initializes the packet checksum fields for DPDK TX offload semantics, and
sets the mbuf `l2_len`, `l3_len`, `l4_len`, and TX offload flags before
transmit:

```bash
sudo ./echo_server_checksum_offload -l 0,1,2,3 -- --checksum-mode=offload 4
```

The server fails clearly if the NIC does not advertise the requested checksum
offload capabilities. It does not silently pretend offload is enabled.

There is also a `--checksum-mode=none` debugging mode. It skips checksum
recomputation and offload preparation, relying on the fact that swapping source
and destination fields preserves the checksum sum for this echo workload. Do not
use this mode as the primary experimental result.

## Data Layout

Store logs with matching names under both modes:

```text
results/raw/checksum/software/server_1c.log
results/raw/checksum/software/server_2c.log
results/raw/checksum/software/server_4c.log
results/raw/checksum/software/server_6c.log
results/raw/checksum/software/server_8c.log
results/raw/checksum/offload/server_1c.log
results/raw/checksum/offload/server_2c.log
results/raw/checksum/offload/server_4c.log
results/raw/checksum/offload/server_6c.log
results/raw/checksum/offload/server_8c.log
```

Generate the comparison plot and CSV:

```bash
python3 analysis/plot_checksum_offload_comparison.py
```

Outputs:

```text
results/processed/checksum_offload_stage_cycles.png
results/processed/checksum_offload_summary.csv
```

## Metrics To Compare

The first metric to inspect is `checksum_cycles_per_pkt`. If checksum is a real
bottleneck, offload mode should reduce this metric sharply because it measures
only software preparation for hardware offload, not full software recomputation.

Then compare `total_cycles_per_pkt`, `rx_mpps`, and `tx_mpps`. A successful
optimization should reduce total cycles per packet and either improve throughput
or improve stability at high core counts.

Finally inspect `rx_cycles_per_pkt`, `tx_cycles_per_pkt`, and
`other_cycles_per_pkt`. If checksum drops but these grow, the bottleneck has
moved to NIC queue pressure, cache/NUMA effects, TX completion behavior, or other
shared resources.

## Correctness Checks

Use `packet_gen_client` as the first correctness check. It should continue to
receive valid echo replies, and client completed echo counts should remain close
to server `tx_pkts`.

For each server log, check:

```bash
rg "tx_drops=[1-9]" results/raw/checksum
```

Small drops near saturation can happen, but unexpected or mode-specific drops
need investigation before trusting the performance result.

Packet checksum validation can be done from the receiving side with tcpdump or
Wireshark. For example, mirror or capture the client-side receive interface and
inspect returned UDP packets:

```bash
sudo tcpdump -i <client-interface> -vvv -nn udp port 50000
```

With hardware checksum offload, packets captured before the NIC completes TX
offload can appear to have incorrect checksums. Capture on the receiving side or
on a tap/mirror after transmission when possible.

## Interpreting Results

Checksum was a bottleneck if offload mode substantially lowers
`checksum_cycles_per_pkt` and also lowers `total_cycles_per_pkt`, especially at
6 or 8 cores.

The bottleneck has likely moved elsewhere if `checksum_cycles_per_pkt` drops but
`total_cycles_per_pkt` stays flat or gets worse. In that case, inspect RX, TX,
and Other cycles, RSS balance, core placement, NUMA locality, burst size, and
ring size next.

# ECE6960 Final project

This repository currently tracks the warmup DPDK echo-server assignment and related code for the ECE6960 project.

## Warmup assignment notes
- The assignment asks us to implement `echo_server.c` so that it immediately echoes each received UDP packet.
- The echoed packet should preserve the payload and swap Ethernet/IP/UDP source and destination headers.
- This repo now includes both the echo server and a packet generator client so a CloudLab node can `git clone`, `make`, and run the experiments directly.

## Files
- `app/echo_server.c`: DPDK echo server implementation
- `app/packet_gen_client.c`: DPDK packet generator for RTT and throughput measurements
- `Makefile`: root-level build entry that compiles both apps

## Quick use on CloudLab
On both nodes:

```bash
git clone <your-repo-url>
cd ECE6960_final_project
make
```

On node 0:

```bash
sudo ./echo_server 10.16.1.1
```

On node 1:

```bash
sudo ./packet_gen_client 64 10.16.1.2 10.16.1.1 <node0-experimental-mac>
```

The client prints:
- RTT summary in microseconds
- throughput summary in Mpps and Gbps

This gives a baseline that you can later analyze and optimize.

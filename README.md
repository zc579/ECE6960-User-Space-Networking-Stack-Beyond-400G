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

## Notes
- Source files now live under `app/`, and the shared header is `app/dpdk.h`.
- The current `echo_server` and `packet_gen_client` follow the starter-code style that uses helpers from `dpdk.h`.
- The experimental NIC selection still depends on the logic inside `app/dpdk.h`, so confirm that `dpdk_port` refers to the experimental interface before running on CloudLab.

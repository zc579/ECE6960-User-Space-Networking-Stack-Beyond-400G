# ECE6960 Final project

This repository currently tracks the warmup DPDK echo-server assignment and related code for the ECE6960 project.

## Files
- `app/echo_server.c`: DPDK echo server implementation
- `app/packet_gen_client.c`: DPDK packet generator for RTT and throughput measurements
- `Makefile`: root-level build entry that compiles both apps

## Quick use on CloudLab
On both nodes:

```bash
git clone git@github.com:zc579/ECE6960-User-Space-Networking-Stack-Beyond-400G.git
cd ECE6960_final_project
make
```

On node 0:

```bash
sudo ./echo_server 10.16.1.1
```

On node 1:

```bash
sudo ./packet_gen_client 64 10.16.1.2 10.16.1.1 ec:b1:d7:85:2a:93
```

The client prints:
- RTT summary in microseconds
- throughput summary in Mpps and Gbps

This gives a baseline that you can later analyze and optimize.

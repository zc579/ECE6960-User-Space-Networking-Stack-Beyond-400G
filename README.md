# ECE6960 Final project

This repository currently tracks the warmup DPDK echo-server assignment and related code for the ECE6960 project.

## Files
- `app/echo_server.c`: DPDK echo server implementation
- `app/packet_gen_client.c`: DPDK packet generator for RTT and throughput measurements
- `app/dpdk.h`: shared DPDK helper definitions used by both programs
- `Makefile`: root-level build entry that compiles both apps

## Hardware setup
The following steps document the 100G hardware bring-up path used on the newer CloudLab environment. This flow assumes the target experimental port is `0000:17:00.0` and the Linux interface is `ens1f0`.

### 1. Confirm machine and NIC status

```bash
uname -r
lspci | grep -i -E 'Ethernet|E810'
ip link
sudo /usr/local/bin/dpdk-devbind.py --status
```

Confirm that `0000:17:00.0` is the target experimental port before continuing.

### 2. Install base dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  meson ninja-build pkg-config \
  libnuma-dev libpcap-dev python3-pyelftools \
  linux-headers-$(uname -r) \
  ethtool pciutils git wget
```

DPDK 22.11 on Linux is built from source with Meson and Ninja. Extra optional components are enabled or disabled depending on which system dependencies are available.

### 3. Install Intel official `ice` driver and DDP package

Intel's `ice` driver README notes that `make install` will also install the default DDP package. During initialization, the driver looks for `intel/ice/ddp/ice.pkg`.

```bash
cd ~
rm -rf ethernet-linux-ice
git clone https://github.com/intel/ethernet-linux-ice.git
cd ethernet-linux-ice/src

make -j"$(nproc)"
sudo make install
```

### 4. Reboot instead of unloading the `ice` module

This ensures:

- the new `ice` driver takes effect
- the new DDP package is loaded
- SSH is not interrupted by `modprobe -r ice`

```bash
sudo reboot
```

### 5. Verify the new driver and DDP package after reconnecting

```bash
ethtool -i ens1f0
ls -l /lib/firmware/updates/intel/ice/ddp/
ls -l /lib/firmware/intel/ice/ddp/
dmesg | grep -i ice | tail -n 100
```

Expected signs:

- `driver: ice`
- `version: 2.5.4`
- `/lib/firmware/updates/intel/ice/ddp/ice.pkg -> ice-1.3.56.0.pkg`
- `The DDP package was successfully loaded`

Intel's out-of-tree `ice` driver prefers firmware files from `/lib/firmware/updates/`.

### 6. Install DPDK 22.11

DPDK 22.11 uses the standard `meson setup build`, `ninja -C build`, and `ninja install` flow, with installation typically landing under `/usr/local`.

```bash
cd ~
rm -rf dpdk
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git fetch --all --tags
git switch -c dpdk-22.11-test v22.11.6

meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

Then confirm the installed version:

```bash
pkg-config --modversion libdpdk
```

You should see `22.11.x`.

### 7. Enable VFIO no-IOMMU mode

DPDK documentation notes that when IOMMU is unavailable, VFIO can still be used if `enable_unsafe_noiommu_mode=1` is enabled.

```bash
sudo modprobe vfio
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

Expected output is `Y` or `1`.

### 8. Bind the target port to `vfio-pci`

First confirm the target port is still using `ice`:

```bash
sudo /usr/local/bin/dpdk-devbind.py --status
```

Then bring the interface down and bind the PCI function:

```bash
sudo ip link set ens1f0 down
sudo /usr/local/bin/dpdk-devbind.py -b vfio-pci 0000:17:00.0
sudo /usr/local/bin/dpdk-devbind.py --status
```

Expected result:

- `0000:17:00.0` appears under `Network devices using DPDK-compatible driver`

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
On node 0:
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

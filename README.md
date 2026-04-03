# ECE6960 Final project

This repository is for our ECE6960 final project on DPDK-based user-space networking beyond 400G.

## Goal
- Select and bring up a minimal network application on top of DPDK as the baseline.
- Break down the application's datapath into multiple execution stages.
- Profile and measure the cost of each stage to understand its performance characteristics.
- Identify the stage that becomes the primary bottleneck for overall performance and scalability.
- Design and propose an optimization or architectural redesign to mitigate that bottleneck.

## Structure
- `app/`: baseline and redesigned datapath code
- `scripts/`: build and run scripts
- `docs/`: setup notes and experiment planning
- `results/`: raw logs and processed figures
- `analysis/`: result parsing and plotting

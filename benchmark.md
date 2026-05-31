# Benchmark

Offline benchmark — normalize + `get_fraud_count` directly, no HTTP overhead, against all 54100 entries in the test dataset.

## Environment

| | |
|---|---|
| **CPU** | AMD Ryzen 7 7700X 8-Core Processor |
| **Cores / Threads** | 8 cores / 16 threads |
| **Max clock** | 5533 MHz |
| **L1d** | 32K |
| **L1i** | 32K |
| **L2**  | 1024K |
| **L3**  | 32768K |
| **Compiler** | GCC (Debian trixie-slim) |
| **Flags** | `-Ofast -march=haswell -mtune=haswell -flto` |
| **Pinned CPUs** | 0 |
| **CPU limit** | 0.37 cores (≈ Core i5-4260U @ 1.4 GHz single-thread) |

> Target hardware is a **Mac mini 2014 (Core i5-4260U, 1.4 GHz)**. The CPU throttle (0.37×) approximates its single-thread performance relative to this machine (~2.7× slower). Use these numbers to compare configs, not to predict absolute latency on the rinha.

## Dataset

| | |
|---|---|
| **Total** | 54100 |
| **Fraud** | 23959 (44.3%) |
| **Legit** | 30141 (55.7%) |
| **Edge cases** | 645 (1.2%) |
| **Borderline (detected)** | 11307 (20.9%) |

## Index

| | |
|---|---|
| **n** | 3000000 |
| **k** | 4096 |
| **train_sample** | 200000 |
| **train_iters** | 69/1000 (converged) |

### Cluster size distribution

> min=13  max=2550  avg=732.4

```mermaid
xychart-beta
    title "Cluster Size Distribution (k=4096, n=3000000)"
    x-axis ["126", "254", "381", "509", "636", "764", "891", "1.0k", "1.1k", "1.3k", "1.4k", "1.5k", "1.7k", "1.8k", "1.9k", "2.0k", "2.2k", "2.3k", "2.4k", "2.6k"]
    y-axis "Clusters" 0 --> 1100
    bar [146, 530, 613, 971, 499, 84, 61, 71, 76, 106, 143, 153, 180, 147, 118, 91, 53, 52, 1, 1]
```

## Results

> `approved = fraud_neighbors / 5 < 0.6` — threshold is fixed by the server.

| NP | NP.BORDER | R.MIN | R.MAX | avg (µs) | p50 (µs) | p99 (µs) | max (µs) | TP | TN | FP | FN | FP% | FN% |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | — | 1 | 4 | 22.05 | 7.65 | 15.52 | 66013.9 | 23959 | 30140 | 1 | 0 | 0.00% | 0.00% |
| 10 | — | 1 | 4 | 25.88 | 9.03 | 18.11 | 66021.6 | 23959 | 30140 | 1 | 0 | 0.00% | 0.00% |
| **12** | **—** | **1** | **4** | **31.29** | **10.94** | **21.64** | **66026.7** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **1** | **12** | **1** | **4** | **13.24** | **4.11** | **16.30** | **65999.0** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| <span style="color:limegreen">**2**</span> | <span style="color:limegreen">**12**</span> | <span style="color:limegreen">**1**</span> | <span style="color:limegreen">**4**</span> | <span style="color:limegreen">**15.05**</span> | <span style="color:limegreen">**4.93**</span> | <span style="color:limegreen">**16.04**</span> | <span style="color:limegreen">**66007.0**</span> | <span style="color:limegreen">**23959**</span> | <span style="color:limegreen">**30141**</span> | <span style="color:limegreen">**0**</span> | <span style="color:limegreen">**0**</span> | <span style="color:limegreen">**0.00%**</span> | <span style="color:limegreen">**0.00%**</span> |
| **3** | **12** | **1** | **4** | **16.96** | **5.81** | **16.39** | **66010.6** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **4** | **12** | **1** | **4** | **18.65** | **6.37** | **16.18** | **66186.5** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **5** | **12** | **1** | **4** | **20.37** | **7.09** | **16.54** | **66189.1** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **6** | **12** | **1** | **4** | **22.20** | **7.77** | **16.92** | **66005.0** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **7** | **12** | **1** | **4** | **23.86** | **8.25** | **17.32** | **66014.0** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **8** | **12** | **1** | **4** | **24.28** | **8.76** | **17.37** | **66194.4** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **9** | **12** | **1** | **4** | **27.20** | **9.27** | **18.04** | **66213.3** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **10** | **12** | **1** | **4** | **27.69** | **9.80** | **18.86** | **66217.1** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |
| **11** | **12** | **1** | **4** | **28.31** | **10.34** | **19.32** | **66282.8** | **23959** | **30141** | **0** | **0** | **0.00%** | **0.00%** |

## Running

```bash
make bench
```

To pin different CPUs, edit `cpuset` in `bench/docker-compose.yml`.

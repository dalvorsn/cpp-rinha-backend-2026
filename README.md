[![Build and Push Rinha Image](https://github.com/dalvorsn/cpp-rinha-backend-2026/actions/workflows/publish.yaml/badge.svg?branch=main)](https://github.com/dalvorsn/cpp-rinha-backend-2026/actions/workflows/publish.yaml)
# Rinha de Backend — 2026

A lightweight, high-performance backend used in the annual "Rinha de Backend" event (an annual backend coding competition). This repository contains a compact C++ service that computes fraud scores using a KNN-like approach over pre-quantized vectors, plus a tiny custom load-balancer.

**What this is**
- A small, optimized C++ backend focused on throughput and low latency.
- Includes a `converter` tool to build a compact binary dataset from JSON references, an `api_server` that serves a `/fraud-score` endpoint, and a minimal `lb` load balancer.
- Intended for the Rinha de Backend event — fast, pragmatic, and a bit playful.

**Repository layout**
- `src/` — source files: `converter.cpp`, `server.cpp`, `lb.cpp`, `knn.hpp`, `normalizer.hpp`, `types.hpp`.
- `resources/` — JSON inputs and example payloads. Place `references.json` (or `references.json.gz`) here.
- `Dockerfile`, `docker-compose.yml` — containerized build and run setup.
- `CMakeLists.txt` — build configuration.

**Build**
```bash
# from repo root
make install-deps
make build

# convert JSON references to binary (if you have references.json or .gz in resources/)
./build/converter resources/references.json resources/references.bin

# run api_server (example)
./build/api_server build/resources/normalization.json resources/mcc_risk.json
```

**Docker**

Build and run with Docker:
```bash
docker-compose up --build
```

**License**
This project is released under the MIT License — see the `LICENSE` file in the repository root for details.

**License (short)**: MIT

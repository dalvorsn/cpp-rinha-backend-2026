FROM debian:trixie-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    gzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc) --target bench converter

RUN if [ -f resources/references.json.gz ]; then \
        gunzip -c resources/references.json.gz > resources/references.json; \
    fi && \
    ./build/converter resources/references.json resources/references.bin

FROM debian:trixie-slim

WORKDIR /app

COPY --from=builder /app/build/bench              /app/bench
COPY --from=builder /app/resources/references.bin /app/resources/references.bin
COPY resources/normalization.json                  /app/resources/
COPY resources/mcc_risk.json                       /app/resources/
COPY test/test-data.json                           /app/test/

VOLUME ["/results"]

# argv: ivf norm mcc data md
ENTRYPOINT ["/bin/sh", "-c", \
    "/app/bench \
    resources/references.bin \
    resources/normalization.json \
    resources/mcc_risk.json \
    test/test-data.json \
    /results/benchmark.md \
    && chmod 666 /results/benchmark.md"]

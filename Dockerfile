FROM debian:trixie-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    gzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

RUN if [ -f resources/references.json.gz ]; then \
        gunzip -c resources/references.json.gz > resources/references.json; \
    fi && \
    ./build/converter resources/references.json resources/references.bin

FROM debian:trixie-slim

WORKDIR /app

COPY --from=builder /app/build/api_server /app/api_server
COPY --from=builder /app/build/lb         /app/lb
COPY --from=builder /app/resources/references.bin  /app/resources/references.bin
COPY resources/mcc_risk.json       /app/resources/
COPY resources/normalization.json  /app/resources/

EXPOSE 9999

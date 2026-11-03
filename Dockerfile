FROM debian:bookworm-slim

RUN --mount=type=cache,target=/var/cache/apt \
    --mount=type=cache,target=/var/lib/apt/lists \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get -o Acquire::Retries=3 update \
    && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
    build-essential clang cmake ninja-build pkg-config python3 \
    libibverbs-dev librdmacm-dev ibverbs-providers ibverbs-utils \
    rdma-core iproute2 kmod ca-certificates

RUN --mount=type=cache,target=/var/cache/apt \
    --mount=type=cache,target=/var/lib/apt/lists \
    apt-get -o Acquire::Retries=3 install -y --no-install-recommends libclang-rt-dev

WORKDIR /work
CMD ["sh"]

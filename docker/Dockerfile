# Simulation toolchain: the same Ubuntu 24.04 packages the CI workflow installs.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        verilator make g++ gdb python3 python3-pil git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work

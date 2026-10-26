# Linux image for running the Iris tests: a C compiler, make, Python and clang.
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev make python3 clang libclang-rt-dev git libsqlite3-dev libssl3t64 ca-certificates curl netcat-openbsd bind9-dnsutils procps openssh-client && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src

# Builds HAProxy's "haload" HTTP/3 load generator. haload is an alternate make target of HAProxy
# that links the whole binary, so it drives load using HAProxy's own QUIC/HTTP-3 stack - a third
# independent stack next to quicly (h3x, h2o-httpclient) and ngtcp2 (h2load). It lives only in
# HAProxy master for now (grooming to replace h1load). debian:trixie ships OpenSSL 3.5, which has
# the native QUIC API, so no aws-lc/quictls or USE_QUIC_OPENSSL_COMPAT is needed.
FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      git make gcc libc6-dev libssl-dev zlib1g-dev ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 https://github.com/haproxy/haproxy && cd haproxy && \
    make -j"$(nproc)" TARGET=linux-glibc USE_OPENSSL=1 USE_QUIC=1 USE_ZLIB=1 haload && \
    install -m0755 haload /usr/local/bin/haload
ENTRYPOINT ["haload"]

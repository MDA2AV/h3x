# Builds h2load with HTTP/3, following nghttp2's official aws-lc recipe
# (README.rst "HTTP/3" section) with pinned, known-good versions. The stock
# Ubuntu h2load has no h3 backend, and quictls's 3.x branches no longer expose
# the QUIC API where nghttp2 looks - aws-lc (a BoringSSL fork) does.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# libc-ares-dev is load-bearing: nghttp2 builds its apps (h2load) only when c-ares is
# found, and h2load's bundled liburlparse.la builds only under that same ENABLE_APP gate.
RUN apt-get update && apt-get install -y --no-install-recommends \
      git make binutils autoconf automake autotools-dev libtool pkg-config perl \
      cmake ninja-build gcc g++ gcc-14 g++-14 zlib1g-dev libev-dev libc-ares-dev ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src

# aws-lc: BoringSSL-derived TLS exposing the QUIC API ngtcp2 + h2load need
RUN git clone --depth 1 -b v5.1.0 https://github.com/aws/aws-lc && cd aws-lc && \
    cmake -B build -DDISABLE_GO=ON -DCMAKE_BUILD_TYPE=Release --install-prefix=/src/aws-lc/opt && \
    make -j"$(nproc)" -C build && cmake --install build

# nghttp3: HTTP/3 framing + QPACK
RUN git clone --depth 1 -b v1.17.0 https://github.com/ngtcp2/nghttp3 && cd nghttp3 && \
    git submodule update --init --depth 1 && \
    ( autoreconf -i || ( rm -rf autom4te.cache && autoreconf -fi ) ) && \
    ./configure --prefix=/src/nghttp3/build --enable-lib-only && \
    make -j"$(nproc)" && make install

# ngtcp2: QUIC transport + crypto helper, built against aws-lc
# ponytail: m4 segfaults sporadically under autoreconf here (transient - does not
# reproduce in isolation); retry once on a clean cache. Switch these steps to
# release dist tarballs (they ship a pre-generated ./configure) if it recurs.
RUN git clone --depth 1 -b v1.24.0 https://github.com/ngtcp2/ngtcp2 && cd ngtcp2 && \
    git submodule update --init --depth 1 && \
    ( autoreconf -i || ( rm -rf autom4te.cache && autoreconf -fi ) ) && \
    ./configure --prefix=/src/ngtcp2/build --enable-lib-only --with-boringssl \
      BORINGSSL_CFLAGS="-I/src/aws-lc/opt/include" \
      BORINGSSL_LIBS="-L/src/aws-lc/opt/lib -lssl -lcrypto" && \
    make -j"$(nproc)" && make install

# nghttp2: build only h2load, HTTP/3 enabled.
# ponytail: this host sporadically SIGSEGVs build tools (m4 in autoreconf above,
# gcc-14's stv pass on llhttp here) - not reproducible in isolation, environmental.
# make is incremental, so a retry recovers by recompiling just the crashed file.
RUN git clone --depth 1 https://github.com/nghttp2/nghttp2 && cd nghttp2 && \
    git submodule update --init --depth 1 && \
    ( autoreconf -i || ( rm -rf autom4te.cache && autoreconf -fi ) ) && \
    ./configure --enable-http3 --disable-shared CC=gcc-14 CXX=g++-14 \
      PKG_CONFIG_PATH="/src/aws-lc/opt/lib/pkgconfig:/src/nghttp3/build/lib/pkgconfig:/src/ngtcp2/build/lib/pkgconfig" \
      LDFLAGS="-Wl,-rpath,/src/aws-lc/opt/lib -Wl,-rpath,/src/nghttp3/build/lib -Wl,-rpath,/src/ngtcp2/build/lib" && \
    for attempt in 1 2 3; do \
      make -j"$(nproc)" -C lib && make -j"$(nproc)" -C third-party && \
      make -j"$(nproc)" -C src h2load && break; \
      echo "nghttp2 make attempt $attempt hit a sporadic segfault; retrying"; \
    done && \
    test -x src/h2load && install -m0755 src/h2load /usr/local/bin/h2load

ENTRYPOINT ["h2load"]

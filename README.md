# h3x
Experimental HTTP/3 load generator using libh2o

As usual, unbuckled seatbelt. Use at your own risk.

## Build

```sh
git submodule update --init        # pulls in deps/h2o (quicly + picotls come with it)
cmake -S . -B build
cmake --build build --target h3x
./build/h3x -h
```

Needs a C toolchain, CMake, and OpenSSL headers. HTTP/3 runs on h2o's evloop
backend, which is what the build links (`libh2o-evloop`).

The `deps/h2o` submodule carries a local patch (UDP GRO on the receive path, +14–19%
against batching servers). It is not upstream, so a `git submodule update` wipes it —
see [`patches/README.md`](patches/README.md) to reapply or revert.

## Usage

```
h3x [options] <url>
```

The run summary — requests, resumed connections, throughput, and latency
percentiles — is printed at the end.

### Load shape

| Flag | Default | Meaning |
|------|---------|---------|
| `-n <count>` | 100 | total requests across all threads |
| `-d <seconds>` | off | run for this long instead of `-n` (overrides `-n`) |
| `-t <num>` | all CPUs | worker threads; auto-detected from the container's CPU set and quota |
| `--connections <n>` | one per thread | total connections, spread across the worker threads |
| `-c <num>` | 10 | concurrent streams per connection |
| `--send-batch <n>` | 1 (off) | hold this many freed slots before refilling, so requests pack into fewer datagrams |

Each worker thread drives one or more QUIC connections over a single UDP socket and event loop,
so `--connections` can far exceed `-t` (for example `-t 8 --connections 1024` puts 128 connections
on each thread). Total requests in flight are `connections * -c`. Leaving `-t` unset uses every CPU
the process is allowed (honoring `--cpuset-cpus` and `--cpus`), which is the intent under Docker.

### Protocol

| Flag | Default | Meaning |
|------|---------|---------|
| `-3 <ratio>` | 100 | HTTP/3 ratio, 0–100 |
| `-2 <ratio>` | 0 | HTTP/2 ratio, 0–100 (remainder falls back to HTTP/1) |

### Request

| Flag | Meaning |
|------|---------|
| `-m <method>` | request method (default `GET`) |
| `-H <name:value>` | add a request header (repeatable) |
| `-x <url>` | connect to this host:port instead of the URL's (pin a backend / skip DNS) |

### TLS

| Flag | Meaning |
|------|---------|
| `-k` | skip certificate verification |
| `--key-exchange <name>` | override the TLS key exchange (e.g. `x25519`) |

Verification uses the system CA bundle (`/etc/ssl/certs/ca-certificates.crt`);
override it with the `H3X_CA_BUNDLE` environment variable.

### Connection reuse / 0-RTT

| Flag | Meaning |
|------|---------|
| `--reconnect <N>` | close each connection after N requests, so connections churn and 0-RTT resumption gets exercised |
| `--no-resumption` | force a full handshake on every connection (disables the ticket cache) |

Resumption is on by default; the summary reports how many connections resumed.

### QUIC transport tuning

| Flag | Meaning |
|------|---------|
| `-W <bytes>` | HTTP/3 receive window, per stream |
| `--max-udp-payload-size <bytes>` | `max_udp_payload_size` transport parameter |
| `--initial-udp-payload-size <bytes>` | initial egress UDP payload size |
| `--ack-frequency <0..1>` | ACK frequency ratio |
| `--disallow-delayed-ack` | disable delayed ACKs |
| `--no-ecn` | disable ECN |
| `--qpack-table <bytes>` | QPACK encoder dynamic table capacity (default 4096) |

## Examples

Basic load — 1000 requests, 50 concurrent per thread, 4 threads:

```sh
./build/h3x -n 1000 -c 50 -t 4 https://example.com/
```

Measure the resumption / 0-RTT benefit — reconnect on every request, with and
without resumption:

```sh
./build/h3x -n 100 --reconnect 1 https://example.com/                  # resumption on
./build/h3x -n 100 --reconnect 1 --no-resumption https://example.com/  # full handshake each time
```

Mixed protocols against a local server with a self-signed cert:

```sh
./build/h3x -3 50 -2 50 -k https://localhost:8443/
```

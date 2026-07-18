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
| `-c <num>` | 10 | concurrent requests in flight, per thread |
| `-t <num>` | 1 | worker threads (each with its own connection + event loop) |

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

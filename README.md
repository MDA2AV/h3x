# h3x
Experimental HTTP/3 load generator using libh2o. As usual, unbuckled seatbelt — use at your own risk.

## Build

```sh
git submodule update --init        # deps/h2o (brings quicly + picotls)
cmake -S . -B build && cmake --build build --target h3x
./build/h3x -h
```

Needs a C toolchain, CMake, and OpenSSL headers; links h2o's evloop backend (`libh2o-evloop`).
`deps/h2o` carries a local patch (UDP GRO on the receive path, +14–19% against batching servers).
It is not upstream, so a `git submodule update` wipes it — see
[`patches/README.md`](patches/README.md) to reapply or revert.

## Usage

```
h3x [options] <url>
```

Each worker thread drives its share of `--connections` over a single UDP socket and event loop, so
connections can far exceed threads (`-t 8 --connections 1024` puts 128 on each thread). Requests in
flight = `connections × -c`. Leaving `-t` unset uses every CPU the process is allowed (honoring
cpusets and quotas — the intent under Docker). The end-of-run summary prints requests, resumed
connections, throughput, and latency percentiles.

| Flag | Meaning (default) |
|------|-------------------|
| `-n <count>` | total requests across all threads (100) |
| `-d <seconds>` | run for a duration instead; overrides `-n` |
| `-t <num>` | worker threads (all allowed CPUs) |
| `--connections <n>` | total connections, spread across threads (one per thread) |
| `-c <num>` | concurrent streams per connection (10) |
| `--send-batch <n>` | hold N freed slots before refilling, so requests pack into fewer datagrams (1 = off) |
| `-m <method>` | request method (`GET`) |
| `-H <name:value>` | add a request header (repeatable) |
| `-x <url>` | connect to this host:port instead of the URL's (pin a backend / skip DNS) |
| `-k` | skip certificate verification |
| `--key-exchange <name>` | override the TLS key exchange (e.g. `x25519`) |
| `--reconnect <N>` | close each connection after N requests, so churn exercises 0-RTT resumption |
| `--no-resumption` | full handshake on every connection (disables the ticket cache) |
| `-W <bytes>` | HTTP/3 receive window, per stream |
| `--max-udp-payload-size <bytes>` | `max_udp_payload_size` transport parameter |
| `--initial-udp-payload-size <bytes>` | initial egress UDP payload size |
| `--ack-frequency <0..1>` | ACK frequency ratio |
| `--disallow-delayed-ack` | disable delayed ACKs |
| `--no-ecn` | disable ECN |
| `--qpack-table <bytes>` | QPACK encoder dynamic table capacity (4096) |

Resumption is on by default. Certificate verification uses the system CA bundle
(`/etc/ssl/certs/ca-certificates.crt`); override with `H3X_CA_BUNDLE`.

## Examples

```sh
./build/h3x -n 1000 -c 50 -t 4 https://example.com/                    # basic load
./build/h3x -n 100 --reconnect 1 https://example.com/                  # resumption / 0-RTT on
./build/h3x -n 100 --reconnect 1 --no-resumption https://example.com/  # vs full handshake each time
```

Benchmarks against h2load across five QUIC servers live in `bench/` (`run.sh`; results in
`results.html`, send-batch A/B in `sb-sweep.sh`).

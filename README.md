# h3x
Experimental HTTP/3 load generator using libh2o. As usual, unbuckled seatbelt. Use at your own risk.

## Build

```sh
git submodule update --init
git -C deps/h2o apply "$(pwd)/patches/h2o-udp-gro-send-batch.patch"  # local h2o patches (patches/README.md)
cmake -S . -B build && cmake --build build --target h3x
```

## Run the servers

```sh
cmake --build build --target h2o                       # once, builds the h2o server binary
build/deps/h2o/h2o -c bench/h2o.conf &                 # h2o (native)      https://127.0.0.1:14433/
docker start nginx-h3 haproxy-h3 caddy-h3 envoy-h3     # 14434 / 14435 / 14436 / 14437
```

Server configs live in `bench/` (bind-mounted into the containers); every server runs 12 workers
and serves the 1 KB `bench/doc_root/index.html`.

## Run the load generator

```sh
./build/h3x -k --connections 512 -m 8 -d 10 https://127.0.0.1:14433/
```

Threads default to one worker per CPU the process may use; connections are spread across the
threads; requests in flight = `connections × -c`.

| Flag | Meaning (default) |
|------|-------------------|
| `-n <count>` | total requests across all threads (100) |
| `-d <seconds>` | run for a duration instead; overrides `-n` |
| `-t <num>` | worker threads (all allowed CPUs) |
| `--connections <n>` | total connections, spread across threads (one per thread) |
| `-m <num>` | concurrent streams per connection (10; h2load calls this `-m` too) |
| `--send-batch <n>` | hold N freed slots before refilling, so requests pack into fewer datagrams (1 = off) |
| `--socket-per-conn` | one UDP socket per connection (unique 4-tuple, the h2load model); avoids servers that kill connections sharing a source port |
| `--requests <file>` | load request templates (method/path/headers/body) from a `.http` file; multiple requests become a round-robin mix (overrides `-m`/`-H`). See `examples/`, and `--dump-requests` to check parsing |
| `--method <method>` | request method (`GET`) |
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

Certificate verification uses the system CA bundle; override with `H3X_CA_BUNDLE`.

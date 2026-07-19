# Local patches to deps/h2o

`deps/h2o` is a pinned submodule, and we carry two small local changes to its HTTP/3
layer that aren't upstream. `git submodule update` wipes them, so they live here as
patch files.

Use **h2o-udp-gro-send-batch.patch** for normal builds: it's the full submodule diff
(`lib/http3/common.c` and `include/h2o/http3_common.h`) with both changes below.
**h2o-udp-gro.patch** is the GRO change on its own, handy for A/B-ing the receive side.
Apply one, not both.

## UDP GRO on receive

Turns on `UDP_GRO` on h2o's QUIC sockets so the kernel coalesces inbound datagrams into
one `recvmsg`, then splits them back apart on QUIC datagram boundaries before decoding.
The split has to respect boundaries because a short-header packet has no length field, so
a naive read would run one packet into the next. It's behind `#ifdef UDP_GRO` and falls
back cleanly on non-Linux or older kernels.

It only helps against servers that GSO-batch their responses, since otherwise there's
nothing to coalesce. Numbers vs the h2o bench server at `-m 32`:

| vs h2o                    | GRO off       | GRO on               |
|---------------------------|---------------|----------------------|
| recv datagrams / request  | 1.63          | 0.55  (3x fewer)     |
| throughput, 1 thread      | 368,753 rps   | 421,621 rps (+14%)   |
| throughput, `-t 8`        | 1,794,334 rps | 2,133,132 rps (+19%) |

No failures against h2o or nginx, single- and multi-thread, or in reconnect mode at 200
connections (which exercises the multi-connection DCID split). Against nginx, which doesn't
batch its sends, GRO can't engage and throughput is flat. Costs about 260 KB of extra
receive buffer per worker; the IPv6 path is untested.

## Cross-connection send batching (`batch_sends`)

Adds a `batch_sends` flag on `h2o_quic_ctx_t`. With it set, instead of one send syscall
per connection each event-loop pass, all of a worker's outgoing datagrams go into a
thread-local buffer (each with its own src-addr/ECN cmsg) and leave in a single `sendmmsg`.
h3x sets it in `worker.c` whenever it's not in `--socket-per-conn` mode, where one socket
per connection leaves nothing to batch.

This merges syscalls, not packets. It pairs with h3x's `--send-batch` knob (plain loadgen
logic, no patch), which holds freed stream slots so several requests start in the same pass
and there's actually a batch to flush.

## Reapply / revert

```sh
# after a submodule update, from the repo root:
git -C deps/h2o apply "$(pwd)/patches/h2o-udp-gro-send-batch.patch"
cmake --build build --target h3x

# regenerate the patch if the diff changes:
git -C deps/h2o diff > patches/h2o-udp-gro-send-batch.patch

# drop the local changes entirely:
git -C deps/h2o checkout lib/http3/common.c include/h2o/http3_common.h
```

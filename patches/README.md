# Local patches to vendored dependencies

`deps/h2o` is a git submodule that we carry **local, un-upstreamed modifications**
to. A `git submodule update` will wipe them, so they live here as patch files and
are reapplied on top of the pinned submodule.

**`h2o-udp-gro-send-batch.patch` is the canonical patch** — the full current
submodule diff (`lib/http3/common.c` + `include/h2o/http3_common.h`), containing
both modifications below. `h2o-udp-gro.patch` is the GRO-only subset, kept for
A/B-ing the receive side in isolation. Apply one or the other, not both.

## UDP GRO on the HTTP/3 receive path

**What it does.** Enables UDP Generic Receive Offload (`UDP_GRO`) on h2o's QUIC
sockets so the kernel coalesces many inbound datagrams into a single `recvmsg`,
then splits that coalesced buffer back into individual datagrams — respecting QUIC
datagram boundaries, since a short-header packet has no length field and would
otherwise be misparsed across a boundary — before decoding. Guarded by
`#ifdef UDP_GRO` with a clean fallback on non-Linux / older kernels.

**Why.** The receive path did one syscall + per-packet work per inbound datagram.
Against a server that GSO-batches its responses (h2o), GRO collapses that cost.
Clean A/B, identical conditions, `-c32` against the h2o benchmark server:

| vs h2o                        | GRO off      | GRO on              |
|-------------------------------|--------------|---------------------|
| receive datagrams / request   | 1.63         | 0.55  (3× fewer)    |
| throughput, single-thread     | 368,753 rps  | 421,621 rps (+14%)  |
| throughput, `-t8`             | 1,794,334 rps| 2,133,132 rps (+19%)|

Correctness verified: 0 request failures against h2o and nginx, multi-thread, and
reconnect mode (200 connections — exercises the multi-connection / DCID split path).

**Known limitation.** GRO only helps when the *server* sends GRO-eligible bursts.
Against a server that doesn't batch its sends (nginx's HTTP/3), it can't engage and
throughput is unchanged. Send-side coalescing is the separate modification below.

**Cost.** ~260 KB extra thread-local memory per worker (larger receive buffers).
IPv6 receive path is untested (the code is address-family-agnostic).

## Cross-connection send batching (`batch_sends`)

**What it does.** Adds a `batch_sends` flag to `h2o_quic_ctx_t`. When set, instead
of flushing each QUIC connection's datagrams with its own syscall per event-loop
pass, every connection's outgoing datagrams are copied into a thread-local buffer
(with a per-message src-addr/ECN cmsg) and flushed with **one `sendmmsg`** at the
end of the pass. h3x always sets it (`loadgen.c`, `h3ctx.h3.batch_sends = 1`).

This is the h2o-layer half of send coalescing; the client-side half is h3x's
`--send-batch <n>` refill knob (pure loadgen.c, no patch), which holds freed
stream slots so requests start together and there is actually something for one
flush to coalesce.

### Reapply (e.g. after a submodule update)
```sh
git -C deps/h2o apply "$(pwd)/patches/h2o-udp-gro-send-batch.patch"   # from repo root
cmake --build build --target h3x
```
If the submodule diff evolves, regenerate with:
`git -C deps/h2o diff > patches/h2o-udp-gro-send-batch.patch`

### Revert (drop all local h2o modifications)
```sh
git -C deps/h2o checkout lib/http3/common.c include/h2o/http3_common.h
cmake --build build --target h3x
```

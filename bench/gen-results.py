#!/usr/bin/env python3
"""Regenerate bench/results.html from the matrix logs, with per-row rank coloring."""
import re, os

B = "/home/diogo/h3x/bench"
SERVERS = [("h2o", "vs h2o (quicly, GRO patch, 12 threads, native)"),
           ("nginx", "vs nginx (OpenSSL-QUIC, docker, 12 workers)"),
           ("haproxy", "vs haproxy (HAProxy QUIC, docker, nbthread 12)")]
LOGS = {  # server -> client -> (file, tag-in-log)
    "h2o":     {"h3x": ("matrix.log", "h3x"), "h2load": ("matrix.log", "h2load"),
                "hc": ("matrix-httpclient.log", "httpclient"), "spc": ("matrix-spc-h2o.log", "h3x-spc")},
    "nginx":   {"h3x": ("matrix-nginx.log", "h3x"), "h2load": ("matrix-nginx.log", "h2load"),
                "hc": ("matrix-httpclient-nginx.log", "httpclient"), "spc": ("matrix-spc-nginx.log", "h3x-spc")},
    "haproxy": {"h3x": ("matrix-haproxy.log", "h3x"), "h2load": ("matrix-haproxy.log", "h2load"),
                "hc": ("matrix-httpclient-haproxy.log", "httpclient"), "spc": ("matrix-spc-haproxy.log", "h3x-spc")},
}
CELLS = [(c, m) for c in (64, 128, 256, 512) for m in (1, 2, 8, 16, 32, 64)]
COLS = ["h3x", "spc", "hc", "h2load"]  # display order

def parse(path, tag):
    out = {}
    rx = re.compile(rf"^RESULT {re.escape(tag)} conns=(\d+) m=(\d+) (\d+) (\d+)")
    with open(path) as f:
        for line in f:
            mt = rx.match(line)
            if mt:
                out[(int(mt.group(1)), int(mt.group(2)))] = (int(mt.group(3)), int(mt.group(4)))
    return out

data = {}
for srv in LOGS:
    data[srv] = {}
    for cli, (fname, tag) in LOGS[srv].items():
        data[srv][cli] = parse(os.path.join(B, fname), tag)

def table(srv):
    rows = []
    for c, m in CELLS:
        vals = {cli: data[srv][cli][(c, m)] for cli in COLS}
        rps = {cli: vals[cli][0] for cli in COLS}
        hi, lo = max(rps.values()), min(rps.values())
        tds = [f"<td>{c}</td><td>{m}</td>"]
        for cli in COLS:
            v = rps[cli]
            cls = "g" if v == hi else ("r" if v == lo else "y")
            pct = v / hi * 100
            ps = f"{pct:.0f}%" if pct >= 9.95 else f"{pct:.1f}%"
            txt = f"<b>{v:,}</b>" if v == hi else f"{v:,}"
            tds.append(f'<td class="{cls}">{txt} <span class="pct">{ps}</span></td>')
        tds.append(f"<td>{vals['h3x'][1]:,}</td><td>{vals['spc'][1]:,}</td>")
        rows.append("<tr>" + "".join(tds) + "</tr>")
    return "\n".join(rows)

HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>h3x vs h2o-httpclient vs h2load</title>
<style>
  :root{color-scheme:light dark}
  body{font:14px/1.5 system-ui,sans-serif;max-width:880px;margin:2rem auto;padding:0 1rem}
  table{border-collapse:collapse;width:100%;margin:1rem 0;table-layout:fixed}
  .pct{opacity:.55;font-size:.85em}
  th,td{text-align:right;padding:.15rem .5rem;border-bottom:1px solid #8884;font-variant-numeric:tabular-nums}
  th{font-weight:600}
  p{opacity:.75;font-size:.93em}
  dl.legend{display:grid;grid-template-columns:max-content 1fr;gap:.3rem 1rem;margin:1rem 0;font-size:.93em}
  dl.legend dt{font-weight:600;white-space:nowrap}
  dl.legend dd{margin:0;opacity:.75}
  details{margin:1rem 0}
  summary{cursor:pointer;font-weight:600;font-size:.93em;opacity:.8}
  .g{background:#22c55e30} .y{background:#eab30826} .r{background:#ef444430}
  input[name=tab]{display:none}
  label{display:inline-block;padding:.3rem .9rem;cursor:pointer;opacity:.6;border-bottom:2px solid transparent}
  .panel{display:none}
  #t1:checked~label[for=t1],#t2:checked~label[for=t2],#t3:checked~label[for=t3]{opacity:1;font-weight:600;border-bottom-color:currentColor}
  #t1:checked~#p1,#t2:checked~#p2,#t3:checked~#p3{display:block}
</style>
</head>
<body>
<h3>h3x vs h2o-httpclient vs h2load: HTTP/3, 1 KB object</h3>
<p>Loopback; i9-14900K, 32 CPUs, nothing pinned or capped. All clients native, 10 s per cell, one
run. h3x and h2load run <code>-t 32</code>; h3x uses <code>--send-batch 64</code>, clamped to m.
Per row: <span class="g">&nbsp;green&nbsp;</span> = fastest client,
<span class="r">&nbsp;red&nbsp;</span> = slowest, <span class="y">&nbsp;yellow&nbsp;</span> = in
between. Raw data: <code>bench/matrix*.log</code>.</p>
<dl class="legend">
<dt>h3x</dt><dd>this project, 32 worker threads, each multiplexing its share of the connections
over one UDP socket (connections share the thread's 4-tuple, distinguished by QUIC connection
ID)</dd>
<dt>h3x spc</dt><dd>the same binary with <code>--socket-per-conn</code>: one UDP socket per
connection, so every connection has a unique 4-tuple; cross-connection sendmmsg batching is off
in this mode</dd>
<dt>h2o-httpclient</dt><dd>h2o's reference client, single-threaded and single-connection; run
here as one process per connection (own socket) with <code>-C</code> = m streams, stopped at
10 s, completions counted from status lines. At 256-512 connections that is 256-512 processes on
32 CPUs (scheduler oversubscribed, fork storm at cell start), so treat that column there as a
floor, not a ceiling</dd>
<dt>h2load</dt><dd>nghttp2's load tool on the ngtcp2 stack, one socket per connection</dd>
</dl>

<input type="radio" name="tab" id="t1" checked>
<input type="radio" name="tab" id="t2">
<input type="radio" name="tab" id="t3">
<label for="t1">h2o</label><label for="t2">nginx</label><label for="t3">haproxy</label>
"""

THEAD = ('<colgroup><col style="width:7%"><col style="width:5%"><col span="4" style="width:18%">'
         '<col span="2" style="width:8%"></colgroup>'
         "<tr><th>conns</th><th>m</th><th>h3x req/s</th><th>h3x spc req/s</th>"
         "<th>h2o-httpclient req/s</th><th>h2load req/s</th><th>h3x drops</th><th>spc drops</th></tr>")

FOOT = """
<details><summary>Notes: methodology, caveats, per-client error behavior</summary>
<p>h2load dropped 0 requests in all 72 cells. The h2load collapse at m=1-2 happens only against h2o
(7k-54k req/s there vs 480k-820k against nginx and 410k-540k against haproxy): a pairing-specific
interaction between ngtcp2 and the quicly server at low concurrency, not general client behavior.
Against nginx the picture inverts for h3x: no GSO-batched responses means its GRO cannot engage,
quicly pays full per-packet receive cost, and h2load leads everywhere above m=1. Against haproxy
h3x and h2load tie within noise on throughput. h2o-httpclient error lines were negligible
(2-7 per cell, process-teardown artifacts) in every cell against nginx and up to 256 conns against
h2o and haproxy; real volumes appeared only at 512 conns against h2o (516 at m=2, 2,051 at m=8,
8,200 at m=32, 16,387 at m=64, uncharacterized by type) and mildly against haproxy (35-195).
The spc column: zero drops in all 24 haproxy cells and nearly all nginx and h2o cells (residuals
at 512 conns are the separate handshake-timeout tail); its throughput cost vs shared-socket mode
is largest against h2o at 128+ conns (up to ~35%, the shared model's home ground: one socket to
poll and batched sends), and roughly par or slightly ahead against haproxy. Single unpinned runs:
differences under ~1.5x are within run-to-run noise.</p>
</details>

<details><summary>Why h3x (shared-socket mode) drops requests: root-caused</summary>
<p>The trigger is multiplexing
many QUIC connections over one UDP socket per worker. Differential tests against haproxy: one
connection per socket is flawless (32 conns / 32 sockets: 7.19M requests, 0 drops, 0 re-dials;
thread-count controlled separately with 4 conns / 4 sockets, also 0/0), while 8 connections per
socket, same 32 connections and same multiplexing, drops 1,428 and re-dials constantly (355k
requests on resumed connections in 5 s, with no reconnect flag). Confirmed at scale by the
h2o-httpclient grids and the spc column above. Servers differ in how they tolerate multiple
QUIC connections sharing a 4-tuple: h2o accepts it (this is h2o's own client architecture), nginx
churns moderately, haproxy constantly. Every drop surfaces as "I/O error" at stream attach: a
shared-socket connection dies, whatever was in flight on it fails, the pool re-dials with the
cached ticket, and the run continues, so throughput barely dips while the drop counter grows. The
close reason on the wire is still unread (CONNECTION_CLOSE is encrypted; reading it needs
SSLKEYLOGFILE wiring), and against nginx there is additionally a smaller classic handshake-timeout
tail under saturation (2,559 churn errors vs 489 timeouts in the probed cell). Fix:
<code>--socket-per-conn</code> (one socket per connection, the h2load model), the spc column.</p>
</details>
</body>
</html>
"""

parts = [HEAD]
for i, (srv, title) in enumerate(SERVERS, 1):
    parts.append(f'\n<div class="panel" id="p{i}">\n<h4>{title}</h4>\n<table>\n{THEAD}\n{table(srv)}\n</table>\n</div>\n')
parts.append(FOOT)

with open(os.path.join(B, "results.html"), "w") as f:
    f.write("".join(parts))
print("rows:", sum(len(data[s]["h3x"]) for s, _ in SERVERS), "cells checked")
for srv, _ in SERVERS:
    for cli in COLS:
        assert len(data[srv][cli]) == 24, (srv, cli, len(data[srv][cli]))
print("OK: 4 clients x 3 servers x 24 cells")

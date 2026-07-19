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

THEAD = ('<colgroup><col style="width:7%"><col style="width:5%"><col span="4" style="width:18%">'
         '<col span="2" style="width:8%"></colgroup>'
         "<tr><th>conns</th><th>m</th><th>h3x req/s</th><th>h3x spc req/s</th>"
         "<th>h2o-httpclient req/s</th><th>h2load req/s</th><th>h3x drops</th><th>spc drops</th></tr>")

# Side-menu panels: key -> (menu label, panel HTML). Benchmark tables are generated; docs are static.
TITLES = {s: t for s, t in SERVERS}
# Shown on top of every benchmark table (not a menu item): what each client column is.
CLIENTS = """<dl class="legend">
<dt>h3x</dt><dd>this project, 32 worker threads, each multiplexing its share of the connections
over one UDP socket (connections share the thread's 4-tuple, distinguished by QUIC connection ID)</dd>
<dt>h3x spc</dt><dd>the same binary with <code>--socket-per-conn</code>: one UDP socket per
connection, so every connection has a unique 4-tuple; cross-connection sendmmsg batching is off in
this mode</dd>
<dt>h2o-httpclient</dt><dd>h2o's reference client, single-threaded and single-connection; run here
as one process per connection (own socket) with <code>-C</code> = m streams, stopped at 10 s,
completions counted from status lines. At 256-512 connections that is 256-512 processes on 32 CPUs
(scheduler oversubscribed, fork storm at cell start), so treat that column there as a floor, not a
ceiling</dd>
<dt>h2load</dt><dd>nghttp2's load tool on the ngtcp2 stack, one socket per connection</dd>
</dl>"""

def bench_panel(srv):
    # CLIENTS legend on top; DETAILS (both defined below) collapse the caveats under the table
    return f'{CLIENTS}\n<h4>{TITLES[srv]}</h4>\n<table>\n{THEAD}\n{table(srv)}\n</table>\n{DETAILS}'

DOC_LAYOUT = """<h4>Source layout &amp; what it reuses from h2o</h4>
<p>h3x is a thin load-generator shell over h2o's client stack. Everything hard (QUIC, TLS, HTTP/3,
the batched UDP I/O) is h2o library code; h3x adds only the load-generation logic on top. It links
one h2o library target, <code>libh2o-evloop</code>, which bundles quicly, picotls, and the HTTP/3
client.</p>
<dl class="legend">
<dt>src/main.c</dt><dd>entry point: CLI parsing, config validation, CPU-count detection (honoring
Docker cpuset/quota), spawns and joins the worker threads</dd>
<dt>src/worker.c</dt><dd>per-thread setup and the event loop: builds the HTTP/3 context (quicly +
picotls), certificate verification, QUIC transport tuning, UDP socket(s) and connection pools; runs
the closed loop with connection-establishment pacing. Both socket modes (shared and
<code>--socket-per-conn</code>) live here</dd>
<dt>src/driver.c</dt><dd>the per-request lifecycle: dispatches requests round-robin across the
worker's connections, the on_connect &rarr; on_head &rarr; on_body callbacks that fill each request
and consume its response, run-budget checks (count or duration), and graceful drain</dd>
<dt>src/requests.c</dt><dd>parser for <code>--requests</code> .http files, turning method / path /
headers / body templates into a round-robin request mix</dd>
<dt>src/tls.c</dt><dd>session-resumption callbacks: the in-memory ticket/token cache that lets
churned connections resume with 0-RTT</dd>
<dt>src/stats.c</dt><dd>per-request latency samples and the merged end-of-run summary (throughput,
percentiles)</dd>
<dt>src/h3x.h</dt><dd>shared config / worker / request structs and the cross-file prototypes</dd>
</dl>
<p><b>Reused from h2o</b> (all via <code>libh2o-evloop</code>): quicly for the QUIC transport;
picotls for TLS 1.3; h2o's <code>lib/http3</code> for HTTP/3 framing and QPACK; its
<code>httpclient.c</code> / <code>http3client.c</code> client state machine, into which h3x's
callbacks plug; and its <code>lib/common</code> event loop (epoll), socket pool, timers, DNS, and
multithread queue. h3x carries one local patch to that library (see <code>patches/</code>): UDP GRO
on the receive path plus cross-connection sendmmsg batching on send. What h3x itself contributes is
only the shared-nothing worker threads, the closed-loop concurrency driver, connection churn with
in-memory 0-RTT resumption, the request-file parser, and merged latency stats.</p>"""

DOC_FINDINGS = """<h4>Key findings</h4>
<p>The workload regime, not the client, decides the winner. Two axes: connection count and streams
per connection (m, the multiplexing depth). h3x is built for a few fat, heavily multiplexed
connections; h2load for many thin ones.</p>
<dl class="legend">
<dt>h3x's home ground</dt><dd>few connections, high m. Its matrix peak is 3.46M req/s at 64
connections x 32 streams against h2o, roughly 2x h2o's own reference client in that cell</dd>
<dt>h2load's home ground</dt><dd>the m=64 column, and any many-thin-connections shape; it takes the
top row in every server's high-m / high-connection cells</dd>
<dt>vs h2o</dt><dd>h2o GSO-batches its responses, so h3x's UDP GRO engages and its receive path
keeps up; h3x leads most cells. At m=1-2 h2load nearly stops (7k-54k req/s) from a
ngtcp2-x-quicly low-concurrency stall specific to that pairing</dd>
<dt>vs nginx</dt><dd>nginx does not GSO-batch, so GRO cannot engage and quicly pays full per-packet
receive cost; the ranking inverts and h2load leads everywhere above m=1</dd>
<dt>vs haproxy</dt><dd>haproxy GSO-batches, so h3x and h2load tie on throughput within noise; the
real difference here is reliability, not speed</dd>
<dt>reliability</dt><dd>default h3x drops requests against nginx and haproxy (the shared-socket
4-tuple churn, see "Why h3x drops requests"); <code>--socket-per-conn</code> removes them (0 drops
in all 24 haproxy cells) at a throughput cost that is largest against h2o</dd>
<dt>server capability</dt><dd>best-client peak per server: h2o 3.66M &gt; nginx 1.65M &gt; haproxy
1.43M req/s</dd>
</dl>"""

DOC_REPRODUCE = """<h4>Reproduce</h4>
<p>Everything runs on one box over loopback. Build h3x and the h2o server, start the servers, then
run the sweep. Full source and scripts are in the repo.</p>
<pre><code># build the client and the h2o server binary
git submodule update --init
git -C deps/h2o apply "$(pwd)/patches/h2o-udp-gro-send-batch.patch"
cmake -S . -B build &amp;&amp; cmake --build build

# start the servers (h2o native on :14433; the rest are containers)
build/deps/h2o/h2o -c bench/h2o.conf &amp;
docker start nginx-h3 haproxy-h3        # :14434 / :14435

# one cell, by hand: 512 connections x 8 streams for 10s against h2o
build/h3x -k -t 32 --connections 512 -m 8 -d 10 https://127.0.0.1:14433/

# the full grid on this page: each script sweeps all three servers and writes bench/matrix*.log
bash bench/matrix.sh              # h3x + h2load
bash bench/matrix-spc.sh          # h3x --socket-per-conn
bash bench/matrix-httpclient.sh   # h2o's reference httpclient
python3 bench/gen-results.py      # rebuilds this page from the logs</code></pre>
<p>Every client here served the same 1 KB <code>bench/doc_root/index.html</code>. Each table cell is
one 10 s run; the raw per-run logs are the <code>bench/matrix*.log</code> files the page is built
from.</p>"""

NOTES_BODY = """<p>h2load dropped 0 requests in all 72 cells. The h2load collapse at m=1-2 happens only against h2o
(7k-54k req/s there vs 480k-820k against nginx and 410k-540k against haproxy): a pairing-specific
interaction between ngtcp2 and the quicly server at low concurrency, not general client behavior.
Against nginx the picture inverts for h3x: no GSO-batched responses means its GRO cannot engage,
quicly pays full per-packet receive cost, and h2load leads everywhere above m=1. Against haproxy
h3x and h2load tie within noise on throughput. h2o-httpclient error lines were negligible
(2-7 per cell, process-teardown artifacts) in every cell against nginx and up to 256 conns against
h2o and haproxy; real volumes appeared only at 512 conns against h2o (516 at m=2, 2,051 at m=8,
8,200 at m=32, 16,387 at m=64, uncharacterized by type) and mildly against haproxy (35-195). The
spc column: zero drops in all 24 haproxy cells and nearly all nginx and h2o cells (residuals at 512
conns are the separate handshake-timeout tail); its throughput cost vs shared-socket mode is
largest against h2o at 128+ conns (up to ~35%, the shared model's home ground: one socket to poll
and batched sends), and roughly par or slightly ahead against haproxy. Single unpinned runs:
differences under ~1.5x are within run-to-run noise.</p>"""

DROPS_BODY = """<p>The trigger is multiplexing many QUIC connections over one UDP socket per worker. Differential
tests against haproxy: one connection per socket is flawless (32 conns / 32 sockets: 7.19M requests,
0 drops, 0 re-dials; thread-count controlled separately with 4 conns / 4 sockets, also 0/0), while
8 connections per socket, same 32 connections and same multiplexing, drops 1,428 and re-dials
constantly (355k requests on resumed connections in 5 s, with no reconnect flag). Confirmed at scale
by the h2o-httpclient grids and the spc column. Servers differ in how they tolerate multiple QUIC
connections sharing a 4-tuple: h2o accepts it (this is h2o's own client architecture), nginx churns
moderately, haproxy constantly. Every drop surfaces as "I/O error" at stream attach: a shared-socket
connection dies, whatever was in flight on it fails, the pool re-dials with the cached ticket, and
the run continues, so throughput barely dips while the drop counter grows. The close reason on the
wire is still unread (CONNECTION_CLOSE is encrypted; reading it needs SSLKEYLOGFILE wiring), and
against nginx there is additionally a smaller classic handshake-timeout tail under saturation (2,559
churn errors vs 489 timeouts in the probed cell). Fix: <code>--socket-per-conn</code> (one socket
per connection, the h2load model), the spc column.</p>"""

# Shown collapsed under each table (not menu items): the running caveats and the drops root-cause.
DETAILS = f"""<details><summary>Methodology &amp; caveats</summary>
{NOTES_BODY}
</details>
<details><summary>Why h3x (shared-socket mode) drops requests</summary>
{DROPS_BODY}
</details>"""

# panel key -> (menu label, html); GROUPS defines sidebar sections and order (first key = default)
PANELS = {
    "h2o": ("h2o", bench_panel("h2o")),
    "nginx": ("nginx", bench_panel("nginx")),
    "haproxy": ("haproxy", bench_panel("haproxy")),
    "findings": ("Key findings", DOC_FINDINGS),
    "layout": ("Source layout", DOC_LAYOUT),
    "reproduce": ("Reproduce", DOC_REPRODUCE),
}
GROUPS = [("Benchmarks", ["h2o", "nginx", "haproxy"]),
          ("Docs", ["findings", "layout", "reproduce"])]
ORDER = [k for _, keys in GROUPS for k in keys]

css_show = "\n".join(
    f"#nav-{k}:checked~.layout #pan-{k}{{display:block}}"
    f"#nav-{k}:checked~.layout label[for=nav-{k}]{{opacity:1;font-weight:600;background:#8882}}"
    for k in ORDER)

STYLE = """  :root{color-scheme:light dark}
  *{box-sizing:border-box}
  body{font:14.5px/1.55 system-ui,sans-serif;margin:0}
  header{max-width:1080px;margin:0 auto;padding:1.3rem 1.2rem .3rem}
  header h3{font-size:1.25rem;margin:.2rem 0}
  header p{max-width:72ch}
  p{opacity:.8;font-size:.94em}
  .layout{display:flex;gap:1.7rem;max-width:1080px;margin:0 auto;padding:.8rem 1.2rem 2.4rem}
  nav.side{width:185px;flex:none;position:sticky;top:1rem;align-self:flex-start}
  nav.side .grp{font-size:.72rem;text-transform:uppercase;letter-spacing:.06em;opacity:.5;margin:1.2rem 0 .3rem}
  nav.side label{display:block;padding:.3rem .6rem;margin:.1rem 0;cursor:pointer;border-radius:6px;opacity:.72;font-size:.95rem}
  nav.side label:hover{background:#8881}
  main{flex:1;min-width:0}
  main h4{font-size:1.08rem;margin:.1rem 0 .7rem}
  table{border-collapse:collapse;width:100%;margin:.4rem 0;table-layout:fixed}
  th,td{text-align:right;padding:.24rem .55rem;border-bottom:1px solid #8884;font-variant-numeric:tabular-nums}
  th{font-weight:600;font-size:.82em;opacity:.85}
  td{font-size:.97em}
  .pct{opacity:.5;font-size:.8em;margin-left:.1em}
  .g{background:#22c55e33} .y{background:#eab30829} .r{background:#ef444433}
  dl.legend{display:grid;grid-template-columns:max-content 1fr;gap:.4rem 1.1rem;font-size:.95em;margin:.9rem 0}
  dl.legend dt{font-weight:600;white-space:nowrap}
  dl.legend dd{margin:0;opacity:.8}
  main p{max-width:76ch}
  pre{background:#8881;padding:.8rem 1rem;border-radius:8px;overflow-x:auto;font-size:.88em;line-height:1.5;max-width:76ch}
  code{background:#8881;padding:.1em .35em;border-radius:4px;font-size:.9em}
  pre code{background:none;padding:0}
  details{margin:1rem 0;border-top:1px solid #8883;padding-top:.6rem}
  summary{cursor:pointer;font-weight:600;font-size:.92rem;opacity:.85}
  details p{margin:.7rem 0 0}
  input[name=nav]{display:none}
  .panel{display:none}
""" + css_show + """
  @media(max-width:720px){
    .layout{flex-direction:column;gap:.4rem;padding:.8rem}
    nav.side{position:static;width:auto;display:flex;flex-wrap:wrap;gap:.2rem;align-items:baseline}
    nav.side .grp{width:100%;margin:.4rem 0 0}
    th,td{padding:.22rem .35rem}
    .pct{display:none}
  }"""

radios = "".join(f'<input type="radio" name="nav" id="nav-{k}"{" checked" if k == ORDER[0] else ""}>'
                  for k in ORDER)
side = []
for grp, keys in GROUPS:
    side.append(f'<div class="grp">{grp}</div>')
    for k in keys:
        side.append(f'<label for="nav-{k}">{PANELS[k][0]}</label>')
side_html = "\n".join(side)
panels_html = "\n".join(f'<section class="panel" id="pan-{k}">\n{PANELS[k][1]}\n</section>' for k in ORDER)

html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>h3x vs h2o-httpclient vs h2load</title>
<style>
{STYLE}
</style>
</head>
<body>
<header>
<h3>h3x vs h2o-httpclient vs h2load: HTTP/3, 1 KB object</h3>
<p>req/s per client, best of one 10 s run. <span class="g">&nbsp;green&nbsp;</span> fastest,
<span class="y">&nbsp;yellow&nbsp;</span> middle, <span class="r">&nbsp;red&nbsp;</span> slowest;
% is share of the fastest. Loopback, i9-14900K, 32 CPUs, unpinned; all clients native, <code>-t
32</code>, h3x <code>--send-batch 64</code>. Raw data: <code>bench/matrix*.log</code>.</p>
</header>
{radios}
<div class="layout">
<nav class="side">
{side_html}
</nav>
<main>
{panels_html}
</main>
</div>
</body>
</html>
"""

with open(os.path.join(B, "results.html"), "w") as f:
    f.write(html)
print("rows:", sum(len(data[s]["h3x"]) for s, _ in SERVERS), "cells checked")
for srv, _ in SERVERS:
    for cli in COLS:
        assert len(data[srv][cli]) == 24, (srv, cli, len(data[srv][cli]))
print("OK: 4 clients x 3 servers x 24 cells; panels:", len(ORDER))

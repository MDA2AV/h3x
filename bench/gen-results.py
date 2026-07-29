#!/usr/bin/env python3
"""Regenerate bench/results.html from the matrix logs, with per-row rank coloring."""
import re, os, math

B = "/home/diogo/h3x/bench"
SERVERS = [("h2o", "vs h2o (quicly, GRO patch, 12 threads, native)"),
           ("nginx", "vs nginx (OpenSSL-QUIC, docker, 12 workers)"),
           ("haproxy", "vs haproxy (HAProxy QUIC, docker, nbthread 12)")]
# One log per server, all four client tags interleaved, all measured in one sitting by
# bench/grid-v3.sh. Earlier revisions of this page mixed a 2026-07-29 h3x column with 2026-07-19
# h2load and httpclient columns; v3 exists so every column is comparable, and so every cell records
# UDP-level drops alongside request-level failures.
LOGS = {srv: f"matrix-v3-{srv}.log" for srv in ("h2o", "nginx", "haproxy")}
TAGS = {"h3x": "h3x", "spc": "h3x-spc", "hc": "httpclient", "h2load": "h2load"}
CELLS = [(c, m) for c in (64, 128, 256, 512) for m in (1, 2, 8, 16, 32, 64)]
COLS = ["h3x", "spc", "hc", "h2load"]  # display order
LABEL = {"h3x": "h3x", "spc": "h3x spc", "hc": "h2o-httpclient", "h2load": "h2load"}

HEAD = re.compile(r"^RESULT (\S+) conns=(\d+) m=(\d+) (\d+) (\d+) ")

def parse(path):
    """-> {(tag, conns, m): record}. Trailing fields are key=value, so new ones stay backward safe."""
    out = {}
    if not os.path.exists(path):
        return out  # let the completeness check below report it, not a traceback from here
    with open(path, errors="replace") as f:
        for line in f:
            # A hard reset mid-run can leave the log NUL-padded (it did on 2026-07-29); grid-v3.sh
            # strips that tail on its next run, but do not depend on having been re-run first.
            line = line.replace("\0", "")
            mt = HEAD.match(line)
            if not mt:
                continue
            kv = dict(p.split("=", 1) for p in line.split() if "=" in p)
            out[(mt.group(1), int(mt.group(2)), int(mt.group(3)))] = {
                "rps": int(mt.group(4)),
                "fail": int(mt.group(5)),
                # -1 marks "counter was unreadable or the socket was recreated mid-cell", which must
                # not be silently rendered as a clean zero.
                "srvdrops": int(kv.get("srvdrops", -1)),
                "clidrops": int(kv.get("clidrops", -1)),
                "mismatch": "OBJSIZE_MISMATCH" in line,
            }
    return out

raw = {srv: parse(os.path.join(B, f)) for srv, f in LOGS.items()}
data = {srv: {cli: {(c, m): raw[srv][(TAGS[cli], c, m)]
                    for c, m in CELLS if (TAGS[cli], c, m) in raw[srv]}
              for cli in COLS}
        for srv in LOGS}

# Check completeness up front. The panels are built at import time and index every cell directly, so
# without this a half-finished grid dies on a bare KeyError after having already overwritten a good
# results.html with a partial one.
missing = [(srv, cli, c, m) for srv in LOGS for cli in COLS for c, m in CELLS
           if (c, m) not in data[srv][cli]]
if missing:
    raise SystemExit(f"incomplete grid: {len(missing)} of {len(LOGS) * len(COLS) * len(CELLS)} "
                     f"cells missing, e.g. {missing[:4]}\n"
                     f"run bench/grid-v3.sh first (it resumes where it stopped)")
mismatched = [(srv, cli, c, m) for srv in LOGS for cli in COLS for c, m in CELLS
              if data[srv][cli][(c, m)]["mismatch"]]
if mismatched:
    raise SystemExit(f"{len(mismatched)} cells flagged OBJSIZE_MISMATCH, e.g. {mismatched[:4]}\n"
                     f"the served object was not {os.path.basename(B)}/doc_root/index.html; "
                     f"do not publish these numbers")

def num(v):
    return f"{v:,}" if v >= 0 else "?"

def si(v):
    """Compact magnitude for stat tiles. Proportional figures, so no padding games."""
    if v >= 1_000_000:
        return f"{v / 1_000_000:.2f}M"
    if v >= 1_000:
        return f"{v / 1_000:.0f}k"
    return f"{v:,}"

# Colour encodes magnitude, not rank. The previous version painted the row winner green, the loser
# red and everything else amber, which is the recolour-on-rank anti-pattern (a cell's colour changed
# meaning depending on what it was sitting next to) and it spent the status palette on something that
# is not a status. Now: one hue per sequential context, light-to-dark with intensity proportional to
# the value, and the status palette is not used at all. Tints are mixed against `transparent` so the
# same percentage composites correctly over either the light or the dark surface.
def tint(pct, hue):  # pct 0..100
    return f'style="background:color-mix(in oklab, var(--seq-{hue}) {pct:.0f}%, transparent)"'

def table_rps(srv):
    """Throughput, tinted by share of the fastest client in that row."""
    rows = []
    for c, m in CELLS:
        rps = {cli: data[srv][cli][(c, m)]["rps"] for cli in COLS}
        hi = max(rps.values())
        tds = [f"<td>{c}</td><td>{m}</td>"]
        for cli in COLS:
            v = rps[cli]
            share = v / hi if hi else 0
            # 0..38% tint: a table cell is a large block, so fills stay light and the ink carries the
            # value. The winner is marked with a bullet as well as tone, so rank is never colour-only.
            mark = ' <span class="win" aria-label="fastest">&#9679;</span>' if v == hi else ""
            ps = f"{share * 100:.0f}%" if share >= 0.0995 else f"{share * 100:.1f}%"
            tds.append(f'<td {tint(share * 38, "a")}>{v:,}{mark}'
                       f'<span class="pct">{ps}</span></td>')
        rows.append("<tr>" + "".join(tds) + "</tr>")
    return "\n".join(rows)

def table_counts(srv, field, ceil):
    """Failures or UDP drops. Zero is the good outcome and gets no tint at all, so a clean grid reads
    as clean at a glance; non-zero is tinted on a log scale because these counts span 1 to 10^6 and a
    linear ramp would render everything below the maximum as blank."""
    rows = []
    for c, m in CELLS:
        tds = [f"<td>{c}</td><td>{m}</td>"]
        for cli in COLS:
            v = data[srv][cli][(c, m)][field]
            if v <= 0:
                cls = "zero" if v == 0 else "unk"
                tds.append(f'<td class="{cls}">{"0" if v == 0 else "?"}</td>')
            else:
                share = math.log10(1 + v) / math.log10(1 + ceil) if ceil > 0 else 0
                tds.append(f'<td {tint(min(share, 1) * 42, "b")}>{v:,}</td>')
        rows.append("<tr>" + "".join(tds) + "</tr>")
    return "\n".join(rows)

def rollup(srv, field, ceil):
    """Failures and drops are overwhelmingly a function of connection count, and 24 rows of mostly
    zeros buries that. This is the same data summed over m: four rows, one per connection count, with
    connections-per-socket spelled out since that is the variable that actually drives it. The full
    per-cell grid stays one click away rather than being the thing you have to scroll past."""
    rows = []
    for c in (64, 128, 256, 512):
        tds = [f'<td>{c}</td><td class="sub2">{c // 32}</td>']
        for cli in COLS:
            v = sum(max(0, data[srv][cli][(c, m)][field]) for m in (1, 2, 8, 16, 32, 64))
            if v == 0:
                tds.append('<td class="zero">0</td>')
            else:
                share = math.log10(1 + v) / math.log10(1 + ceil * 6) if ceil > 0 else 0
                tds.append(f'<td {tint(min(share, 1) * 42, "b")}>{v:,}</td>')
        rows.append("<tr>" + "".join(tds) + "</tr>")
    return ('<div class="scroll"><table><colgroup><col style="width:7%"><col style="width:9%">'
            '<col span="4" style="width:21%"></colgroup>'
            '<tr><th>conns</th><th>per socket</th>'
            + "".join(f"<th>{LABEL[c]}</th>" for c in COLS) + "</tr>\n"
            + "\n".join(rows) + "</table></div>")

def thead(unit):
    return ('<colgroup><col style="width:7%"><col style="width:5%">'
            '<col span="4" style="width:22%"></colgroup>'
            "<tr><th>conns</th><th>m</th>"
            + "".join(f"<th>{LABEL[c]}{unit}</th>" for c in COLS) + "</tr>")

def scale(hue, lo, hi, note):
    """Sequential encodings need a scale legend; without one the tint is decoration."""
    swatches = "".join(
        f'<i {tint(i / 6 * (38 if hue == "a" else 42), hue)}></i>' for i in range(7))
    return (f'<div class="scale"><span>{lo}</span><div class="ramp">{swatches}</div>'
            f'<span>{hi}</span><em>{note}</em></div>')

def tiles(srv):
    """The tables answer "which cell"; nobody should have to read 72 rows to learn who won. Peak,
    cells won, failures and drops are the four numbers the page actually argues about."""
    best = max(COLS, key=lambda c: max(data[srv][c][k]["rps"] for k in CELLS))
    peak = max(data[srv][best][k]["rps"] for k in CELLS)
    peak_at = max(CELLS, key=lambda k: data[srv][best][k]["rps"])
    wins = {}
    for k in CELLS:
        w = max(COLS, key=lambda c: data[srv][c][k]["rps"])
        wins[w] = wins.get(w, 0) + 1
    fails = {c: sum(data[srv][c][k]["fail"] for k in CELLS) for c in COLS}
    drops = {c: sum(max(0, data[srv][c][k]["srvdrops"]) for k in CELLS) for c in COLS}
    worst_f = max(COLS, key=lambda c: fails[c])
    worst_d = max(COLS, key=lambda c: drops[c])
    won = ", ".join(f"{LABEL[c]} {n}" for c, n in
                    sorted(wins.items(), key=lambda kv: -kv[1]))
    def tile(label, value, sub):
        return (f'<div class="tile"><div class="tl">{label}</div>'
                f'<div class="tv">{value}</div><div class="ts">{sub}</div></div>')
    return ('<div class="tiles">'
            + tile("fastest client", LABEL[best],
                   f"{si(peak)} req/s at {peak_at[0]} conns, m={peak_at[1]}")
            + tile("cells won", f"{max(wins.values())}<span class='of'>/24</span>", won)
            + tile("most failed requests",
                   si(fails[worst_f]) if fails[worst_f] else "none",
                   f"{LABEL[worst_f]}; total across all clients {si(sum(fails.values()))}"
                   if fails[worst_f] else "every client, every cell")
            + tile("most server UDP drops",
                   si(drops[worst_d]) if drops[worst_d] else "none",
                   f"{LABEL[worst_d]}; total across all clients {si(sum(drops.values()))}"
                   if drops[worst_d] else "no receive-buffer overflow at all")
            + "</div>")

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

# Why three tables rather than one wide one. Throughput, failed requests and dropped datagrams are
# not variations on a theme, they are independent axes, and on this box they point opposite ways: h2o
# discards tens of thousands of datagrams per cell while failing zero requests (QUIC retransmits and
# the client never notices), whereas haproxy discards none and fails hundreds. Putting them in one
# table invited reading a big drop count as a bad result, which is exactly backwards.
MEASURES = """<dl class="legend">
<dt>req/s</dt><dd>completed responses per second. This is the only table where the four clients are
directly comparable, so it is the only one rank-coloured green-to-red across the row</dd>
<dt>failed requests</dt><dd>requests the client gave up on. Against h3x in shared-socket mode these
are the 4-tuple churn described under "Why h3x drops requests"; the connection dies, whatever was in
flight on it fails, and the pool re-dials</dd>
<dt>server UDP drops</dt><dd>datagrams the kernel discarded on the <em>server's</em> socket because
its receive buffer was full, read per cell from <code>/proc/net/udp</code>. Every drop we measured
was a receive-buffer overflow (the per-socket counter, snmp <code>RcvbufErrors</code> and snmp
<code>InErrors</code> were exactly equal). These cost throughput, not correctness: QUIC retransmits
them. All three servers run the stock 212992-byte buffer (<code>net.core.rmem_max</code>), so this
column is a drain-rate difference, not a buffer-size one</dd>
</dl>"""

def bench_panel(srv):
    # Tiles first (the answer), then the three tables (the evidence), then the caveats, collapsed.
    fail_hi = max(data[srv][c][k]["fail"] for c in COLS for k in CELLS)
    drop_hi = max(data[srv][c][k]["srvdrops"] for c in COLS for k in CELLS)
    return (f'<h4>{TITLES[srv]}</h4>\n{tiles(srv)}\n'
            f'<details class="key"><summary>What the four clients are, and what each table '
            f'measures</summary>\n{CLIENTS}\n{MEASURES}\n</details>\n'
            f'<h5>throughput</h5>\n'
            f'{scale("a", "0%", "100%", "share of the fastest client in that row")}\n'
            f'<div class="scroll"><table>\n{thead(" req/s")}\n{table_rps(srv)}\n</table></div>\n'
            f'<h5>failed requests <span class="sub">summed over m</span></h5>\n'
            f'{scale("b", "0", "max", "log scale; untinted means zero")}\n'
            f'{rollup(srv, "fail", fail_hi)}\n'
            f'<details><summary>per-cell breakdown, all 24 cells</summary>\n'
            f'<div class="scroll"><table>\n{thead("")}\n'
            f'{table_counts(srv, "fail", fail_hi)}\n</table></div></details>\n'
            f'<h5>server UDP datagrams dropped <span class="sub">receive-buffer overflow, '
            f'summed over m</span></h5>\n'
            f'{scale("b", "0", "max", "log scale; untinted means zero")}\n'
            f'{rollup(srv, "srvdrops", drop_hi)}\n'
            f'<details><summary>per-cell breakdown, all 24 cells</summary>\n'
            f'<div class="scroll"><table>\n{thead("")}\n'
            f'{table_counts(srv, "srvdrops", drop_hi)}\n</table></div></details>\n{DETAILS}')

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
<dt>h3x's home ground</dt><dd>few connections, high m. Its matrix peak is 3.54M req/s at 64
connections x 64 streams against h2o, 1.9x h2o's own reference client in that cell</dd>
<dt>h2load's home ground</dt><dd>many thin connections, and low m generally. It takes 22 of 24 nginx
cells and wins nothing at all against haproxy</dd>
<dt>vs h2o</dt><dd>h2o GSO-batches its responses, so h3x's UDP GRO engages and its receive path
keeps up; h3x takes 21 of 24 cells. At m=1-2 h2load nearly stops (5k-14k req/s) from a
ngtcp2-x-quicly low-concurrency stall specific to that pairing, recovering to 3.39M by m=64</dd>
<dt>vs nginx</dt><dd>nginx does not GSO-batch, so GRO cannot engage and quicly pays full per-packet
receive cost; the ranking inverts and h2load leads 22 of 24 cells</dd>
<dt>vs haproxy</dt><dd>haproxy GSO-batches, and here the h3x family takes every cell: h2load and the
reference client win none. The two socket modes split it evenly, 12 cells each</dd>
<dt>failed requests</dt><dd>shared-socket h3x fails requests against haproxy (53,372 over the grid)
and nginx (3,914); h2load fails none anywhere. This is the 4-tuple churn described under "Why h3x
drops requests", and it scales sharply with connections per socket: against haproxy, 8.6 failures
per million at 2 connections per socket rising to 737.9 at 16. <code>--socket-per-conn</code>
removes it against nginx entirely and all but eliminates it against haproxy (1,904, versus 53,372),
at a throughput cost of +16.1% against h2o and +25.0% against nginx but -1.9% against haproxy</dd>
<dt>UDP drops are a separate axis</dt><dd>and it runs the other way. h2o and nginx discard large
numbers of datagrams to receive-buffer overflow while failing zero requests; haproxy discards
essentially none while failing the most. The client matters more than the server here: h2load causes
7.5 server drops per million requests against h2o where h3x causes 951.5, and 160.4 against nginx
where h3x causes 11,242.4</dd>
<dt>server capability</dt><dd>best-client peak per server: h2o 3.54M (h3x) &gt; nginx 1.73M
(h2load) &gt; haproxy 1.45M (h3x spc) req/s</dd>
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

# one cell, by hand: 512 connections x 8 streams for 10s against h2o, send-batch = half the
# worker target ((512/32) x 8 / 2 = 64)
build/h3x -k -t 32 --connections 512 -m 8 -d 10 --send-batch 64 https://127.0.0.1:14433/

# the whole grid on this page: all four clients x three servers x 24 cells, with per-cell UDP
# drop accounting. Resumable - re-running appends only the cells a log is missing and leaves
# finished grids alone. Writes bench/matrix-v3-<server>.log. Takes about 75 minutes.
bash bench/grid-v3.sh

# a subset, for one cell or one column
CONNS=512 MS=64 bash bench/grid-v3.sh

python3 bench/gen-results.py      # rebuilds this page from the logs</code></pre>
<p>Every client here served the same 1 KB <code>bench/doc_root/index.html</code>. Each table cell is
one 10 s run; the raw per-run logs are the <code>bench/matrix-v3-*.log</code> files the page is
built from, one per server with all four client tags interleaved.</p>"""

NOTES_BODY = """<p>h2load failed 0 requests in all 72 cells, on every server. Its collapse at m=1-2 happens only
against h2o (5k-14k req/s there, recovering to 3.39M by m=64): a pairing-specific interaction
between ngtcp2 and the quicly server at low concurrency, not general client behaviour. Against nginx
the picture inverts for h3x: no GSO-batched responses means its GRO cannot engage, quicly pays full
per-packet receive cost, and h2load leads 22 of 24 cells. Against haproxy the h3x family leads all
24 and both rivals win nothing. h2o-httpclient's failures stay negligible throughout (134, 51 and
131 over the three grids, process-teardown artifacts). The spc column costs +16.1% mean against h2o
and +25.0% against nginx, and gains 1.9% against haproxy, where shared-socket mode is bleeding
requests in the first place.</p>
<p><b>All 288 cells are one sitting</b> (2026-07-29, bench/grid-v3.sh), same box, same servers, same
1 KB object, so columns are directly comparable. Every cell re-derives the served object size from
the client's own byte counters and the page refuses to build if any cell disagrees with the 1 KB
file, because an earlier round of sweeps was silently invalidated by benchmarking a 20 KB object
against 1 KB tables.</p>
<p><b>Single unpinned runs.</b> One 10 s run per cell, no repeats. A control experiment across two
grids, comparing cells whose configuration was byte-identical, saw swings from -17.6% to +13.5% with
no consistent sign. So differences under roughly 20% here are not distinguishable from run-to-run
variance, and only the large effects (the drop counts, the m=1-2 h2load collapse, the nginx
inversion) should be read as real. Separating anything smaller needs repeated samples per cell.</p>"""

DROPS_BODY = """<p>The trigger is multiplexing many QUIC connections over one UDP socket per worker. Differential
tests against haproxy: one connection per socket is flawless (32 conns / 32 sockets: 7.19M requests,
0 drops, 0 re-dials; thread-count controlled separately with 4 conns / 4 sockets, also 0/0), while
8 connections per socket, same 32 connections and same multiplexing, drops 1,428 and re-dials
constantly (355k requests on resumed connections in 5 s, with no reconnect flag). Confirmed at scale by the spc column, which fails 0 requests against nginx and 1,904 against
haproxy versus shared-socket h3x's 3,914 and 53,372. Servers differ in how they tolerate multiple QUIC
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
    f"#nav-{k}:checked~.layout label[for=nav-{k}]"
    f"{{color:var(--ink);font-weight:600;background:color-mix(in oklab,var(--seq-a) 12%,transparent);border-color:color-mix(in oklab,var(--seq-a) 30%,transparent)}}"
    for k in ORDER)

# Palette roles, not raw hex, so light and dark swap in one place. Values are the reference
# data-viz palette: sequential blue for throughput, sequential orange for the two count tables (the
# documented second sequential hue). The status palette is deliberately absent -- nothing here is a
# good/bad state, it is all magnitude, and spending status colours on magnitude is what the previous
# version got wrong. Dark steps are chosen for the dark surface, not flipped from light.
STYLE = """  :root{
    color-scheme:light dark;
    --surface:#fcfcfb; --plane:#f9f9f7;
    --ink:#0b0b0b; --ink-2:#52514e; --ink-muted:#898781;
    --rule:#e1e0d9; --edge:rgba(11,11,11,.10);
    --seq-a:#2a78d6; --seq-b:#eb6834;
  }
  @media(prefers-color-scheme:dark){:root:where(:not([data-theme=light])){
    --surface:#1a1a19; --plane:#0d0d0d;
    --ink:#fff; --ink-2:#c3c2b7; --ink-muted:#898781;
    --rule:#2c2c2a; --edge:rgba(255,255,255,.10);
    --seq-a:#3987e5; --seq-b:#d95926;
  }}
  :root[data-theme=dark]{
    --surface:#1a1a19; --plane:#0d0d0d;
    --ink:#fff; --ink-2:#c3c2b7; --ink-muted:#898781;
    --rule:#2c2c2a; --edge:rgba(255,255,255,.10);
    --seq-a:#3987e5; --seq-b:#d95926;
  }
  *{box-sizing:border-box}
  body{font:14.5px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;
       background:var(--plane);color:var(--ink)}
  header{max-width:1140px;margin:0 auto;padding:1.8rem 1.2rem .2rem}
  header h3{font-size:1.55rem;margin:0 0 .5rem;letter-spacing:-.02em}
  header .lede{max-width:82ch;font-size:1.02em;color:var(--ink-2);margin:0 0 1.1rem}
  main p{max-width:76ch;color:var(--ink-2);font-size:.94em}
  /* The run configuration is metadata, not prose: as a full-width spec strip it uses the horizontal
     space the old two-paragraph header wasted, and each fact becomes individually findable. */
  .spec{display:grid;grid-template-columns:repeat(auto-fit,minmax(172px,1fr));gap:0;margin:0;
        border-top:1px solid var(--rule);border-bottom:1px solid var(--rule)}
  .spec>div{padding:.6rem .9rem;border-left:1px solid var(--rule)}
  .spec>div:first-child{border-left:0;padding-left:0}
  @media(max-width:1180px){.spec>div{border-left:0;padding-left:0}}
  .spec dt{font-size:.68rem;text-transform:uppercase;letter-spacing:.07em;color:var(--ink-muted)}
  .spec dd{margin:.15rem 0 0;font-size:.92rem;color:var(--ink);line-height:1.35}
  .spec dd span{display:block;font-size:.78rem;color:var(--ink-muted);margin-top:.1rem}
  .spec code{font-size:.85em}
  td.sub2{color:var(--ink-muted)}
  .layout{display:flex;gap:1.9rem;max-width:1140px;margin:0 auto;padding:.8rem 1.2rem 3rem}
  nav.side{width:190px;flex:none;position:sticky;top:1rem;align-self:flex-start}
  nav.side .grp{font-size:.7rem;text-transform:uppercase;letter-spacing:.07em;
                color:var(--ink-muted);margin:1.3rem 0 .35rem}
  nav.side label{display:block;padding:.34rem .6rem;margin:.1rem 0;cursor:pointer;
                 border-radius:7px;color:var(--ink-2);font-size:.95rem;
                 border:1px solid transparent}
  nav.side label:hover{background:color-mix(in oklab,var(--seq-a) 8%,transparent)}
  main{flex:1;min-width:0}
  main h4{font-size:1.1rem;margin:.1rem 0 .9rem;letter-spacing:-.01em}
  main h5{font-size:.83rem;text-transform:uppercase;letter-spacing:.07em;
          color:var(--ink-muted);margin:1.8rem 0 .1rem;font-weight:600}
  main h5 .sub{text-transform:none;letter-spacing:0;font-weight:400;opacity:.75}
  /* Stat tiles: the answer, above the evidence. Proportional figures on the big number. */
  .tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:.7rem;
         margin:.2rem 0 1.4rem}
  .tile{background:var(--surface);border:1px solid var(--edge);border-radius:10px;padding:.7rem .85rem}
  .tile .tl{font-size:.7rem;text-transform:uppercase;letter-spacing:.06em;color:var(--ink-muted)}
  .tile .tv{font-size:1.5rem;line-height:1.25;margin:.15rem 0 .1rem;letter-spacing:-.02em}
  .tile .tv .of{font-size:.9rem;color:var(--ink-muted);letter-spacing:0}
  .tile .ts{font-size:.79rem;color:var(--ink-2);line-height:1.4}
  /* Wide tables scroll inside their own box; the page body never scrolls sideways. */
  .scroll{overflow-x:auto;background:var(--surface);border:1px solid var(--edge);
          border-radius:10px;margin:.5rem 0 0}
  table{border-collapse:collapse;width:100%;min-width:560px;table-layout:fixed}
  th,td{text-align:right;padding:.3rem .6rem;font-variant-numeric:tabular-nums;
        border-bottom:1px solid var(--rule)}
  tr:last-child td{border-bottom:0}
  th{font-weight:600;font-size:.78em;color:var(--ink-muted);white-space:nowrap;
     position:sticky;top:0;background:var(--surface)}
  td{font-size:.95em;color:var(--ink)}
  tbody tr:hover td,table tr:hover td{outline:1px solid color-mix(in oklab,var(--seq-a) 35%,transparent);
                                      outline-offset:-1px}
  .pct{color:var(--ink-muted);font-size:.78em;margin-left:.35em}
  .win{color:var(--seq-a);font-size:.7em;vertical-align:.15em;margin-left:.25em}
  td.zero{color:var(--ink-muted)}
  td.unk{color:var(--ink-muted);font-style:italic}
  /* Sequential encodings get a scale legend, or the tint is just decoration. */
  .scale{display:flex;align-items:center;gap:.45rem;margin:.5rem 0 0;
         font-size:.75rem;color:var(--ink-muted);flex-wrap:wrap}
  .scale .ramp{display:flex;border:1px solid var(--edge);border-radius:4px;overflow:hidden}
  .scale i{width:20px;height:11px;display:block}
  .scale em{font-style:normal;opacity:.85}
  dl.legend{display:grid;grid-template-columns:max-content 1fr;gap:.45rem 1.1rem;
            font-size:.93em;margin:.9rem 0}
  dl.legend dt{font-weight:600;white-space:nowrap;color:var(--ink)}
  dl.legend dd{margin:0;color:var(--ink-2)}
  pre{background:var(--surface);border:1px solid var(--edge);padding:.85rem 1rem;border-radius:10px;
      overflow-x:auto;font-size:.86em;line-height:1.55;max-width:80ch}
  code{background:color-mix(in oklab,var(--ink) 7%,transparent);padding:.1em .35em;
       border-radius:4px;font-size:.9em}
  pre code{background:none;padding:0}
  details{margin:1.1rem 0;border-top:1px solid var(--rule);padding-top:.7rem}
  details.key{border-top:0;padding-top:0;margin:.2rem 0 .4rem}
  summary{cursor:pointer;font-weight:600;font-size:.9rem;color:var(--ink-2)}
  summary:hover{color:var(--ink)}
  details p{margin:.7rem 0 0}
  input[name=nav]{position:absolute;opacity:0;pointer-events:none}
  input[name=nav]:focus-visible~.layout label[for]{outline:2px solid var(--seq-a);outline-offset:2px}
  .panel{display:none}
""" + css_show + """
  @media(max-width:760px){
    .layout{flex-direction:column;gap:.4rem;padding:.8rem}
    nav.side{position:static;width:auto;display:flex;flex-wrap:wrap;gap:.2rem;align-items:baseline}
    nav.side .grp{width:100%;margin:.5rem 0 0}
    th,td{padding:.26rem .4rem}
    .tiles{grid-template-columns:1fr 1fr}
  }
  @media(prefers-reduced-motion:reduce){*{transition:none!important}}"""

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
<p class="lede">Four HTTP/3 clients over a grid of connections x streams-per-connection, against
three servers. Every cell records throughput, failed requests, and the datagrams the kernel dropped
on each side, because those three point in different directions and only reading all of them gets
the story right.</p>
<dl class="spec">
<div><dt>grid</dt><dd>4 conns x 6 streams x 4 clients x 3 servers<span>288 cells, one 10 s run each</span></dd></div>
<div><dt>machine</dt><dd>i9-14900K, 32 CPUs<span>loopback, unpinned, all clients native</span></dd></div>
<div><dt>object</dt><dd>1 KB static file<span>re-derived per cell from the client's own byte counters</span></dd></div>
<div><dt>h3x send-batch</dt><dd>half the worker target<span><code>conns/threads x m / 2</code>, not a fixed 64</span></dd></div>
<div><dt>colour</dt><dd>magnitude, deepest = largest<span>scale under every table; the row winner also carries a marker</span></dd></div>
<div><dt>raw data</dt><dd><code>bench/matrix-v3-*.log</code><span>one per server, four client tags interleaved</span></dd></div>
</dl>
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
cells = len(LOGS) * len(COLS) * len(CELLS)
nodrop = sum(1 for s in LOGS for c in COLS for k in CELLS if data[s][c][k]["srvdrops"] < 0)
print(f"OK: {cells} cells (4 clients x 3 servers x 24), all object-size checked; panels: {len(ORDER)}")
if nodrop:
    print(f"note: {nodrop} cells have no UDP drop data (counter unreadable), shown as '?'")

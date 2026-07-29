#!/usr/bin/env bash
# The v3 grid: all four clients, all three servers, one sitting, with UDP drop accounting.
#
# Why a third grid rather than more patching of matrix*.sh. The v2 logs carried only h3x and h3x-spc;
# the h2load and h2o-httpclient columns were still from 2026-07-19, so results.html was comparing
# columns measured ten days and one reboot apart. And none of the logs recorded UDP-level drops at
# all, which turned out to matter: h2o discards 40k-190k datagrams to receive-buffer overflow in a
# 5s cell while failing zero requests, and haproxy discards zero datagrams while failing ~1000. Those
# are opposite failure modes and the old logs could not tell them apart. This grid measures every
# client under identical conditions and records both numbers per cell.
#
# Output: bench/matrix-v3-<server>.log, one per server, all four client tags interleaved.
#
# Re-running is safe and resumes: it appends only the cells a log is missing and leaves finished
# grids alone. The box has frozen mid-run under this load before (2026-07-29 10:05, hard reset,
# NUL-padded log tail), so a crash must cost one cell, not the whole grid.
#
# Servers must already be running, the same ones the earlier grids used:
#   h2o     build/h2o-gro-only -c bench/h2o.conf   (NOT build/deps/h2o/h2o, which carries our edit)
#   nginx   docker start nginx-h3
#   haproxy docker start haproxy-h3
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."

D=${D:-10} T=${T:-32} COOL=${COOL:-5}
# Overridable so a subset can be re-run by hand (CONNS=512 MS=64 bash bench/grid-v3.sh). CELLS stays
# the full-grid count regardless, so a partial run can never write a premature ALL DONE.
CONNS=${CONNS:-"64 128 256 512"}
MS=${MS:-"1 2 8 16 32 64"}
CELLS=96  # 4 clients x 4 conns x 6 m, per server

# PAYLOAD picks the object. Each size lives at its own URL rather than being swapped into one file,
# so the request itself names the payload: a run cannot quietly measure the wrong object, which is
# how a previous round of sweeps was invalidated (a 20 KB object recorded against 1 KB tables).
# haproxy needs a matching `http-request return ... if { path ... }` rule per payload in haproxy.conf,
# since unlike h2o and nginx it does not resolve paths against doc_root itself.
PAYLOAD=${PAYLOAD:-1k}
case "$PAYLOAD" in
    1k)   REQPATH="/";           OBJFILE=bench/doc_root/index.html ;;
    20k)  REQPATH="/20k.html";   OBJFILE=bench/doc_root/20k.html ;;
    128k) REQPATH="/128k.html";  OBJFILE=bench/doc_root/128k.html ;;
    *) echo "unknown PAYLOAD=$PAYLOAD (want 1k, 20k or 128k)" >&2; exit 1 ;;
esac
# bench/doc_root is gitignored, so generate the payload rather than requiring it to be checked in:
# a clone can reproduce the grid without shipping a 128 KB fixture. Deterministic and exact-sized.
if [ ! -f "$OBJFILE" ]; then
    case "$PAYLOAD" in 20k) WANT=20480 ;; 128k) WANT=131072 ;; *) WANT=1024 ;; esac
    mkdir -p bench/doc_root
    python3 - "$OBJFILE" "$WANT" <<'PYGEN'
import sys
path, size = sys.argv[1], int(sys.argv[2])
head = f'<!doctype html><title>{size}B</title><p>h3x benchmark payload, {size} bytes exactly.</p>\n'
body = head + "<!-- " + "x" * (size - len(head) - len("<!--  -->\n")) + " -->\n"
assert len(body) == size, (len(body), size)
open(path, "w").write(body)
PYGEN
    echo "generated $OBJFILE ($WANT B)" >&2
fi
OBJ=$(stat -c%s "$OBJFILE")
HC=build/deps/h2o/h2o-httpclient
ulimit -n 65536 2>/dev/null || true

[ -x build/h3x ] || { echo "build h3x first"; exit 1; }
[ -x bench/h2load ] || { echo "bench/h2load missing"; exit 1; }
[ -x "$HC" ] || { echo "$HC missing"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Derive the served object size from MB/s and req/s. The 2026-07 sweeps were invalidated by silently
# benchmarking a 20 KB object against 1 KB tables, so every cell that reports bytes re-checks it.
objcheck() { # bytes -> "objbytes=N ok" | "objbytes=N OBJSIZE_MISMATCH"
    # 10%, floored at 40B. The guard is for contamination (the smallest possible mix-up here is 20x
    # off), not for the few percent of drift that cells with a heavy in-flight tail at the cutoff show
    # in both directions. gen-results.py re-derives this from the recorded objbytes and reports
    # 2-10% separately as drift, so nothing is hidden by the looser flag.
    local bytes=${1:-0} tol=$((OBJ / 10))
    [ "$tol" -lt 40 ] && tol=40
    if [ "$bytes" -lt $((OBJ - tol)) ] || [ "$bytes" -gt $((OBJ + tol)) ]; then
        echo "objbytes=$bytes OBJSIZE_MISMATCH"
    else
        echo "objbytes=$bytes ok"
    fi
}

objflag() { # rps mbs -> for clients whose MB/s is body throughput (h3x)
    local rps=$1 mbs=$2 bytes
    if [ "${rps:-0}" -gt 0 ] && [ -n "$mbs" ]; then
        bytes=$(python3 -c "print(round($mbs*1048576/$rps))" 2>/dev/null || echo 0)
    else
        bytes=0
    fi
    objcheck "$bytes"
}

# Each client runner echoes "<rps> <fail> <objbytes=N flag>" and nothing else.
run_h3x() { # conns m extra-flags...
    local C=$1 M=$2; shift 2
    local cpw=$((C / T)) target sb out rps fail mbs
    [ "$cpw" -lt 1 ] && cpw=1
    target=$((cpw * M)); sb=$((target / 2)); [ "$sb" -lt 1 ] && sb=1
    out=$(build/h3x -k -t $T --connections "$C" -m "$M" -d $D --send-batch "$sb" "$@" "$URL" 2>&1)
    rps=$(printf '%s' "$out" | awk '/throughput:/{print $2+0}')
    mbs=$(printf '%s' "$out" | awk '/throughput:/{print $4+0}')
    fail=$(printf '%s' "$out" | awk '/completed:/{print $4+0}')
    echo "${rps:-0} ${fail:-0} $(objflag "${rps:-0}" "${mbs:-0}") sb=$sb"
}

run_h2load() { # conns m
    local C=$1 M=$2 out rps fail data ok2xx bytes
    out=$(bench/h2load --alpn-list=h3 -c "$C" -m "$M" -t $T -D $D -n 100000000 "$URL" 2>&1)
    # "finished in 10.00s, 123456.78 req/s, 120.56MB/s"
    rps=$(printf '%s' "$out" | awk -F'[ ,]+' '/finished in/{printf "%d", $4+0}')
    # "requests: N total, N started, N done, N succeeded, N failed, N errored, N timeout"
    fail=$(printf '%s' "$out" | awk -F'[ ,]+' '/^requests:/{print $10+0}')
    # h2load's MB/s counts headers and QUIC framing, so deriving the object size from it lands around
    # 1100 on a 1024B file and trips a false mismatch. Its traffic line reports body bytes on their
    # own; pair those with the 2xx count (which is what those bytes correspond to, not "done").
    data=$(printf '%s' "$out" | sed -nE 's/.*\(([0-9]+)\) data.*/\1/p')
    ok2xx=$(printf '%s' "$out" | sed -nE 's/^status codes: ([0-9]+) 2xx.*/\1/p')
    if [ -n "$data" ] && [ -n "$ok2xx" ] && [ "$ok2xx" -gt 0 ]; then
        bytes=$((data / ok2xx))
    else
        bytes=0
    fi
    echo "${rps:-0} ${fail:-0} $(objcheck "$bytes")"
}

run_httpclient() { # conns m
    local C=$1 M=$2 i
    rm -f "$TMP"/c.*
    for i in $(seq "$C"); do
        ( timeout "$D" "$HC" -3 100 -k -t 99999999 -C "$M" -o /dev/null "$URL" 2>&1 >/dev/null \
          | awk '/^HTTP\/3 200/{ok++;next} /^HTTP\//{bad++;next} /^[a-zA-Z0-9-]+: /{next} /^$/{next} {bad++} END{print ok+0, bad+0}' > "$TMP/c.$i" ) &
    done
    wait
    # No bytes reported by this client, so the object-size check cannot apply; say so rather than
    # printing a fabricated objbytes=0 that looks like a mismatch.
    awk -v d="$D" '{o+=$1;b+=$2} END{printf "%d %d objbytes=n/a ok", o/d, b+0}' "$TMP"/c.*
}

cell() { # tag conns m runner-fn extra-args...
    local tag=$1 C=$2 M=$3 fn=$4; shift 4
    local body drops
    python3 bench/udpstat.py snap "$TMP/before.json"
    body=$("$fn" "$C" "$M" "$@")
    python3 bench/udpstat.py snap "$TMP/after.json"
    drops=$(python3 bench/udpstat.py delta "$TMP/before.json" "$TMP/after.json" "$PORT")
    echo "RESULT $tag conns=$C m=$M $body $drops t=$(date +%T)"
}

logged() {
    [ -f "$1" ] || { echo 0; return 0; }
    tr -d '\000' < "$1" | grep -c '^RESULT '
    return 0
}

grid() { # logfile
    local log=$1
    if [ -s "$log" ]; then
        tr -d '\000' < "$log" > "$log.tmp" && mv "$log.tmp" "$log"
    else
        {
            echo "GRID-V3 START $(date +%F' '%T) payload=$PAYLOAD obj=${OBJ}B server=$NAME url=$URL t=$T d=$D unpinned"
            echo "# h3x send-batch = half the worker target ((conns/threads) x m / 2), clamp removed"
            echo "# srvdrops = server socket rcvbuf overflow on $PORT; clidrops = global RcvbufErrors minus that"
        } >> "$log"
    fi
    local done_cells n
    done_cells=$(grep -o "^RESULT [a-z0-9-]* conns=[0-9]* m=[0-9]* " "$log")
    n=$(logged "$log")
    [ "$n" -gt 0 ] && echo "RESUME $NAME: $n/$CELLS cells already logged" >&2
    {
        for C in $CONNS; do
            for M in $MS; do
                case "$done_cells" in *"RESULT h3x conns=$C m=$M "*) ;; *)
                    cell h3x "$C" "$M" run_h3x; sleep $COOL ;; esac
                case "$done_cells" in *"RESULT h3x-spc conns=$C m=$M "*) ;; *)
                    cell h3x-spc "$C" "$M" run_h3x --socket-per-conn; sleep $COOL ;; esac
                case "$done_cells" in *"RESULT h2load conns=$C m=$M "*) ;; *)
                    cell h2load "$C" "$M" run_h2load; sleep $COOL ;; esac
                case "$done_cells" in *"RESULT httpclient conns=$C m=$M "*) ;; *)
                    cell httpclient "$C" "$M" run_httpclient; sleep $COOL ;; esac
            done
            grep -q "^CONNS-DONE $C\$" "$log" || echo "CONNS-DONE $C"
        done
    } >> "$log" 2>&1
    if [ "$(logged "$log")" -eq "$CELLS" ] && ! grep -q '^ALL DONE' "$log"; then
        echo "ALL DONE $(date +%T)" >> "$log"
    fi
}

for entry in "h2o 14433" "nginx 14434" "haproxy 14435"; do
    set -- $entry; NAME=$1; PORT=$2; URL="https://127.0.0.1:$PORT$REQPATH"
    LOG="bench/matrix-v3-$NAME-$PAYLOAD.log"
    if [ "$(logged "$LOG")" -eq "$CELLS" ]; then
        echo "DONE $NAME $PAYLOAD: grid already complete" >&2
        continue
    fi
    if ! timeout 5 build/h3x -k -n 1 -t 1 --connections 1 "$URL" >/dev/null 2>&1; then
        echo "SKIP $NAME ($URL): not reachable" >&2
        continue
    fi
    echo "=== $NAME $PAYLOAD ($URL) ===" >&2
    grid "$LOG"
done
echo "GRID COMPLETE $PAYLOAD $(date +%F' '%T)" >&2

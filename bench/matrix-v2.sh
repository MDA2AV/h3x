#!/usr/bin/env bash
# Re-run the h3x columns after the --send-batch clamp fix (main.c) and the QUIC_READ_DGRAMS /
# QUIC_READ_MAX_SEGS change (patches/h2o-udp-gro-send-batch.patch).
#
# Why a v2 script rather than editing matrix.sh: the old logs must survive. gen-results.py reads the
# h2load column out of matrix.log and the h2o-httpclient column out of matrix-httpclient*.log, and
# neither client changed, so those numbers carry over. Only the h3x and h3x-spc columns are stale.
# New results go to bench/matrix-v2-*.log; nothing existing is touched.
#
# send-batch policy: the old grid passed --send-batch 64, which main.c silently clamped to min(64, m),
# so five of the six m columns ran with a much smaller batch than the logs claimed. Unclamped, a fixed
# 64 means wildly different things per cell because the worker-wide target is (conns/threads) * m.
# This grid therefore sets it per cell to half the worker target, the regime that measured best at
# 256 conns / m=8 (sb=32 beat both full drain and the old effective sb=8).
#
# Re-running this script is safe and resumes: it appends only the cells a log is missing, so an
# interrupted grid picks up where it stopped and a finished one is left alone.
#
# Servers must already be running and must be the SAME ones the Jul 19 grid used:
#   h2o     build/h2o-gro-only -c bench/h2o.conf   (NOT build/deps/h2o/h2o, which now carries our edit)
#   nginx   docker start nginx-h3
#   haproxy docker start haproxy-h3
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."

D=10 T=32 COOL=5
OBJ=$(stat -c%s bench/doc_root/index.html)
ulimit -n 65536 2>/dev/null || true

[ -x build/h3x ] || { echo "build h3x first"; exit 1; }

# Pull req/s, failures and MB/s out of a summary, then derive the served object size. The Jul 2026
# sweeps were invalidated by silently benchmarking a 20 KB object against 1 KB tables, so every cell
# re-derives bytes = MB/s * 1048576 / (req/s) and the row is flagged if it does not match OBJ.
cell() { # binary-args... -> "RESULT ..." fields
    local out rps fail mbs bytes flag
    out=$("$@" 2>&1)
    rps=$(printf '%s' "$out" | awk '/throughput:/{print $2+0}')
    mbs=$(printf '%s' "$out" | awk '/throughput:/{print $4+0}')
    fail=$(printf '%s' "$out" | awk '/completed:/{print $4+0}')
    if [ "${rps:-0}" -gt 0 ]; then
        bytes=$(python3 -c "print(round($mbs*1048576/$rps))" 2>/dev/null || echo 0)
    else
        bytes=0
    fi
    flag=ok
    [ "$bytes" -lt $((OBJ - 40)) ] || [ "$bytes" -gt $((OBJ + 40)) ] && flag="OBJSIZE_MISMATCH($bytes)"
    printf '%d %d mbs=%s objbytes=%s %s' "${rps:-0}" "${fail:-0}" "$mbs" "$bytes" "$flag"
}

CELLS=24  # 4 conns x 6 m

# Count the cells a log already holds, tolerating a NUL-padded tail (see grid()). Always prints
# exactly one integer: grep -c prints 0 but exits 1 on no match, so it must not gate an || fallback.
logged() {
    [ -f "$1" ] || { echo 0; return 0; }
    tr -d '\000' < "$1" | grep -c '^RESULT '
    return 0
}

grid() { # tag logfile extra-h3x-flags...
    local tag=$1 log=$2; shift 2
    # Resume-safe, and deliberately never truncates. The box froze mid-cell at 10:05 on 2026-07-29 and
    # was hard-reset, which left matrix-v2-spc-nginx.log holding 16 good cells plus a 96-byte NUL tail
    # from writes that were never flushed. So: strip any NUL tail, then skip cells already recorded.
    # A re-run after a crash costs only the interrupted cell, and a finished log survives untouched.
    if [ -s "$log" ]; then
        tr -d '\000' < "$log" > "$log.tmp" && mv "$log.tmp" "$log"
    else
        {
            echo "MATRIX-V2 START $(date +%F' '%T) obj=${OBJ}B tag=$tag t=$T d=$D sb=half-worker-target unpinned"
            echo "# h3x build: send-batch clamp removed (worker-wide threshold); QUIC_READ_DGRAMS=10, MAX_SEGS=DGRAMS*64"
        } >> "$log"
    fi
    local done_cells n_before
    done_cells=$(grep -o "^RESULT $tag conns=[0-9]* m=[0-9]* " "$log")
    n_before=$(logged "$log")
    [ "$n_before" -gt 0 ] && echo "RESUME $tag: $n_before/$CELLS cells already logged" >&2
    {
        for C in 64 128 256 512; do
            for M in 1 2 8 16 32 64; do
                case "$done_cells" in *"RESULT $tag conns=$C m=$M "*) continue ;; esac
                local cpw=$((C / T)) target sb
                [ "$cpw" -lt 1 ] && cpw=1
                target=$((cpw * M)); sb=$((target / 2)); [ "$sb" -lt 1 ] && sb=1
                echo "RESULT $tag conns=$C m=$M $(cell build/h3x -k -t $T --connections "$C" -m "$M" -d $D --send-batch "$sb" "$@" "$URL") sb=$sb target=$target t=$(date +%T)"
                sleep $COOL
            done
            grep -q "^CONNS-DONE $C\$" "$log" || echo "CONNS-DONE $C"
        done
    } >> "$log" 2>&1
    if [ "$(logged "$log")" -eq "$CELLS" ] && ! grep -q '^ALL DONE' "$log"; then
        echo "ALL DONE $(date +%T)" >> "$log"
    fi
}

for entry in "h2o https://127.0.0.1:14433/" "nginx https://127.0.0.1:14434/" "haproxy https://127.0.0.1:14435/"; do
    set -- $entry; NAME=$1; URL=$2
    # Check completeness before the reachability probe, so a finished server's grid does not force us
    # to keep that server running just to be told there is nothing left to do.
    if [ "$(logged "bench/matrix-v2-$NAME.log")" -eq "$CELLS" ] &&
       [ "$(logged "bench/matrix-v2-spc-$NAME.log")" -eq "$CELLS" ]; then
        echo "DONE $NAME: both grids already complete" >&2
        continue
    fi
    if ! timeout 5 build/h3x -k -n 1 -t 1 --connections 1 "$URL" >/dev/null 2>&1; then
        echo "SKIP $NAME ($URL): not reachable" >&2
        continue
    fi
    echo "=== $NAME shared-socket ===" >&2
    grid h3x     "bench/matrix-v2-$NAME.log"
    echo "=== $NAME socket-per-conn ===" >&2
    grid h3x-spc "bench/matrix-v2-spc-$NAME.log" --socket-per-conn
done
echo "GRID COMPLETE $(date +%F' '%T)" >&2

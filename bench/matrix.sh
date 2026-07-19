#!/usr/bin/env bash
# h3x vs h2load matrix over all three servers. Native, no taskset, unpinned. Each cell:
# conns {64,128,256,512} x streams {1,2,8,16,32,64}, -t 32, -d 10s, h3x --send-batch 64.
# Servers must already be running (see the Reproduce section of bench/results.html).
# Writes matrix.log (h2o), matrix-nginx.log, matrix-haproxy.log - the logs gen-results.py reads.
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."
D=10 T=32 COOL=5
ulimit -n 65536 2>/dev/null || true

run_cell() { # name url conns m  -> "RESULT h3x ..." then "RESULT h2load ..."
    local name=$1 url=$2 C=$3 M=$4 out
    out=$(build/h3x -k -t $T --connections "$C" -m "$M" -d $D --send-batch 64 "$url" 2>&1)
    echo "RESULT h3x conns=$C m=$M $(printf '%s' "$out" | awk '/throughput:/{r=$2} /completed:/{f=$4} END{printf "%d %d", r+0, f+0}') t=$(date +%T)"
    sleep $COOL
    out=$(bench/h2load --alpn-list=h3 -c "$C" -m "$M" -t $T -D $D -n 100000000 "$url" 2>&1)
    echo "RESULT h2load conns=$C m=$M $(printf '%s' "$out" | awk -F'[ ,]+' '/finished in/{r=$4} /requests:/{f=$10} END{printf "%.0f %d", r+0, f+0}') t=$(date +%T)"
    sleep $COOL
}

for entry in "h2o https://127.0.0.1:14433/" "nginx https://127.0.0.1:14434/" "haproxy https://127.0.0.1:14435/"; do
    set -- $entry; NAME=$1 URL=$2
    LOG="bench/matrix.log"; [ "$NAME" != h2o ] && LOG="bench/matrix-$NAME.log"
    [ -f "$LOG" ] && mv "$LOG" "$LOG.old"
    {
        echo "MATRIX START $(date +%F' '%T) obj=$(stat -c%s bench/doc_root/index.html)B server=$NAME t=$T d=$D h3x-send-batch=64 unpinned"
        for C in 64 128 256 512; do
            for M in 1 2 8 16 32 64; do run_cell "$NAME" "$URL" "$C" "$M"; done
            echo "CONNS-DONE $C"
        done
        echo "ALL DONE $(date +%T)"
    } >> "$LOG" 2>&1
done

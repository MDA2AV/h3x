#!/usr/bin/env bash
# h3x --socket-per-conn grid over all three servers (one UDP socket per connection). Same shape as
# bench/matrix.sh: conns {64..512} x streams {1..64}, -t 32, -d 10s, --send-batch 64.
# Writes matrix-spc-{h2o,nginx,haproxy}.log - the spc columns gen-results.py reads.
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."
D=10 T=32 COOL=5
ulimit -n 65536 2>/dev/null || true

for entry in "h2o https://127.0.0.1:14433/" "nginx https://127.0.0.1:14434/" "haproxy https://127.0.0.1:14435/"; do
    set -- $entry; NAME=$1 URL=$2
    LOG="bench/matrix-spc-$NAME.log"
    [ -f "$LOG" ] && mv "$LOG" "$LOG.old"
    {
        echo "MATRIX START $(date +%F' '%T) obj=$(stat -c%s bench/doc_root/index.html)B client=h3x--socket-per-conn server=$NAME t=$T d=$D sb=64 unpinned"
        for C in 64 128 256 512; do
            for M in 1 2 8 16 32 64; do
                out=$(build/h3x -k -t $T --connections "$C" -c "$M" -d $D --send-batch 64 --socket-per-conn "$URL" 2>&1)
                echo "RESULT h3x-spc conns=$C m=$M $(printf '%s' "$out" | awk '/throughput:/{r=$2} /completed:/{f=$4} END{printf "%d %d", r+0, f+0}') t=$(date +%T)"
                sleep $COOL
            done
            echo "CONNS-DONE $C"
        done
        echo "ALL DONE $(date +%T)"
    } >> "$LOG" 2>&1
done

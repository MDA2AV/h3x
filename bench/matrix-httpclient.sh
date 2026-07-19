#!/usr/bin/env bash
# h2o's reference httpclient over the same grid, as one process (one connection, one UDP socket)
# per connection, -C = streams, 10s cells, completions counted from response status lines. Over all
# three servers. Writes matrix-httpclient.log (h2o), matrix-httpclient-{nginx,haproxy}.log.
# Note: at 256-512 connections this forks that many single-threaded processes onto the box.
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."
D=10 COOL=4
BIN=build/deps/h2o/h2o-httpclient
ulimit -n 65536 2>/dev/null || true
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

for entry in "h2o https://127.0.0.1:14433/" "nginx https://127.0.0.1:14434/" "haproxy https://127.0.0.1:14435/"; do
    set -- $entry; NAME=$1 URL=$2
    LOG="bench/matrix-httpclient.log"; [ "$NAME" != h2o ] && LOG="bench/matrix-httpclient-$NAME.log"
    [ -f "$LOG" ] && mv "$LOG" "$LOG.old"
    {
        echo "MATRIX START $(date +%F' '%T) obj=$(stat -c%s bench/doc_root/index.html)B client=h2o-httpclient(1 proc+socket per conn) server=$NAME d=$D unpinned"
        for C in 64 128 256 512; do
            for M in 1 2 8 16 32 64; do
                rm -f "$T"/c.*
                for i in $(seq "$C"); do
                    ( timeout "$D" "$BIN" -3 100 -k -t 99999999 -C "$M" -o /dev/null "$URL" 2>&1 >/dev/null \
                      | awk '/^HTTP\/3 200/{ok++;next} /^HTTP\//{bad++;next} /^[a-zA-Z0-9-]+: /{next} /^$/{next} {bad++} END{print ok+0, bad+0}' > "$T/c.$i" ) &
                done
                wait
                awk -v c="$C" -v m="$M" -v d="$D" '{o+=$1;b+=$2} END{printf "RESULT httpclient conns=%d m=%d %d %d\n", c, m, o/d, b}' "$T"/c.*
                sleep "$COOL"
            done
            echo "CONNS-DONE $C"
        done
        echo "ALL DONE $(date +%T)"
    } >> "$LOG" 2>&1
done

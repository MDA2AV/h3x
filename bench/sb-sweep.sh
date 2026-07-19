#!/usr/bin/env bash
# send-batch A/B (1 vs 8) x {512,1024} conns x 8 streams x 5 servers x 3 runs, with per-run CPU.
#   lg=  cores h3x used ((user+sys)/real);  srv= cores the server used over the same window
#   (containers: cgroup cpu.stat delta; h2o: /proc utime+stime delta, HZ=100)
# Check the object size first: ls -la bench/doc_root/  (results.html tables assume 1 KB).
# Log is durable and sync'd per cell. Override via env: CONNS= M= D= RUNS= COOLDOWN= SERVERS= LOG=
# Pinning is up to the caller: run bare for unbounded, or `taskset -c ...` this script for pinned.
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.."
LOG="${LOG:-bench/sb-sweep.log}"
M="${M:-8}" D="${D:-10}" COOLDOWN="${COOLDOWN:-6}" RUNS="${RUNS:-3}"
CONNS="${CONNS:-512 1024}"
SERVERS="${SERVERS:-h2o=https://127.0.0.1:14433/ nginx=https://127.0.0.1:14434/ haproxy=https://127.0.0.1:14435/ caddy=https://127.0.0.1:14436/ envoy=https://127.0.0.1:14437/}"

H2OPID=$(pgrep -f 'deps/h2o/h2o' | head -1)
declare -A CID
for s in nginx haproxy caddy envoy; do CID[$s]=$(docker inspect -f '{{.Id}}' "$s-h3"); done

parse() { awk '/throughput:/{r=$2} /completed:/{f=$4} END{printf "%d %d", r+0, f+0}'; }
temp()  { t=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -rn | head -1); echo "${t:-0}"; }
srv_usec() { # cumulative server cpu in usec
    if [ "$1" = h2o ]; then awk '{print ($14+$15)*10000}' "/proc/$H2OPID/stat"
    else awk '/^usage_usec/{print $2}' "/sys/fs/cgroup/system.slice/docker-${CID[$1]}.scope/cpu.stat"
    fi
}

TIMEFORMAT='TIMES %U %S %R'
{
    echo "SWEEP START $(date +%F' '%T) obj=$(stat -c%s bench/doc_root/index.html 2>/dev/null)B M=$M D=$D RUNS=$RUNS h2opid=$H2OPID"
    for entry in $SERVERS; do
        name="${entry%%=*}" url="${entry#*=}"
        for conns in $CONNS; do
            for b in 1 8; do
                for run in $(seq "$RUNS"); do
                    s0=$(srv_usec "$name")
                    out=$( { time build/h3x -k --connections "$conns" -c "$M" -d "$D" --send-batch "$b" "$url" 2>&1; } 2>&1 )
                    s1=$(srv_usec "$name")
                    rf=$(printf '%s' "$out" | parse)
                    lgreal=$(printf '%s\n' "$out" | awk '/^TIMES/{u=$2;s=$3;r=$4} END{if(r+0>0) printf "%.1f %.2f",(u+s)/r,r; else print "0 1"}')
                    lg=${lgreal% *}; real=${lgreal#* }
                    srv=$(awk -v a="$s0" -v b="$s1" -v r="$real" 'BEGIN{printf "%.1f",(b-a)/1e6/r}')
                    echo "RESULT $name conns=$conns batch=$b run=$run $rf lg=$lg srv=$srv temp=$(temp) t=$(date +%T)"
                    sync
                    sleep "$COOLDOWN"
                done
            done
        done
        echo "SERVER-DONE $name"
        sync
    done
    echo "ALL DONE $(date +%T)"
    sync
} >> "$LOG" 2>&1

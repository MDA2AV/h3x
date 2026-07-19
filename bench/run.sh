#!/usr/bin/env bash
# Benchmark HTTP/3 load generators (h3x vs h2load) against several h3 servers, each a
# different QUIC stack: h2o (quicly), nginx (OpenSSL-QUIC), haproxy (HAProxy QUIC),
# caddy (quic-go), envoy (Google quiche).
#
# CPU split (this box: 8 P-cores = CPUs 0-15 w/ SMT, 16 E-cores = CPUs 16-31):
#   servers   -> 6 P-cores, CPUs 0-11 (12 SMT threads). Pin them AT LAUNCH (not here):
#                  docker run --cpuset-cpus 0-11 ...       (nginx/haproxy/caddy/envoy)
#                  taskset -c 0-11 build/deps/h2o/h2o ...  (h2o, on the host)
#   load gen  -> the rest, CPUs 12-31 (20 threads). This script pins the clients there:
#                  h3x   via  taskset -c 12-31
#                  h2load via docker --cpuset-cpus 12-31
#   h3x auto-detects its thread count from the cpuset (one worker per CPU), so no -t.
#
# Duration based: each C in CONNS runs C connections x M streams for D seconds. Servers
# with far fewer worker threads (12) than the client are the point: this is the many-
# connection regime. Override with CONNS=, M=, D=, SERVERS="name=url ...", or one URL=.
set -euo pipefail
cd "$(dirname "$0")/.."

CONNS="${CONNS:-512 1024}"          # connection counts to sweep
M="${M:-8}"                          # concurrent streams per connection
D="${D:-10}"                         # duration in seconds
COOLDOWN="${COOLDOWN:-6}"            # settle between runs so the server drains lingering QUIC conns
CLIENT_CPUS="${CLIENT_CPUS:-12-31}"  # load-generator cores (servers get the rest, 0-11)

if [ -n "${URL:-}" ]; then
    SERVERS="target=$URL"            # single explicit target
else
    SERVERS="${SERVERS:-h2o=https://127.0.0.1:14433/ nginx=https://127.0.0.1:14434/ haproxy=https://127.0.0.1:14435/ caddy=https://127.0.0.1:14436/ envoy=https://127.0.0.1:14437/}"
fi

[ -x build/h3x ] || { echo "build h3x first:  cmake --build build"; exit 1; }
docker image inspect h2load-h3 >/dev/null 2>&1 || {
    echo "image 'h2load-h3' missing - build it:  docker build -t h2load-h3 - < bench/h2load-h3.Dockerfile"; exit 1; }

hr() { printf '==================================================================\n'; }

# Pull "<req/s> <failed>" out of each client's summary, for the RESULT lines below.
parse_h3x()    { awk '/throughput:/{r=$2} /completed:/{f=$4} END{printf "%d %d", r+0, f+0}'; }
parse_h2load() { awk -F'[ ,]+' '/finished in/{r=$4} /requests:/{f=$10} END{printf "%.0f %d", r+0, f+0}'; }

run_h3x() {    # conns url  -> h3x, pinned to the client cores, threads auto-detected
    taskset -c "$CLIENT_CPUS" build/h3x -k --connections "$1" -m "$M" -d "$D" "$2" 2>&1
}
run_h2load() { # conns url  -> h2load h3, pinned to the client cores
    local t=$(( $1 < 20 ? $1 : 20 ))   # h2load requires threads <= connections
    docker run --rm --network host --cpuset-cpus "$CLIENT_CPUS" --ulimit nofile=1048576:1048576 \
        h2load-h3 --alpn-list=h3 -c "$1" -m "$M" -t "$t" -D "$D" -n 100000000 "$2" 2>&1
}

bench_server() { # name url
    local name="$1" url="$2" conns out
    hr; echo "  SERVER: $name   ($url)"; hr
    for conns in $CONNS; do
        echo "-- $conns connections x $M streams, ${D}s --"
        echo ">>> h3x    (taskset -c $CLIENT_CPUS, auto threads, --connections $conns -m $M)"
        out=$(run_h3x "$conns" "$url" || true); echo "$out" | grep -E 'throughput:|completed:' || true
        echo "RESULT $name h3x $conns $M $(printf '%s' "$out" | parse_h3x)"
        sleep "$COOLDOWN"
        echo ">>> h2load (--cpuset-cpus $CLIENT_CPUS, -t 20, -c $conns -m $M)"
        out=$(run_h2load "$conns" "$url" || true); echo "$out" | grep -E 'finished in|requests:' || true
        echo "RESULT $name h2load $conns $M $(printf '%s' "$out" | parse_h2load)"
        sleep "$COOLDOWN"
        echo
    done
}

for entry in $SERVERS; do
    bench_server "${entry%%=*}" "${entry#*=}"
done

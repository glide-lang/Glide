#!/usr/bin/env bash
# Compare Glide HTTP variants against nginx + Axum on the same box.
# Pinned wrk + multi-run median. Run from repo root.
#
# Each server gets 4 workers and binds 127.0.0.1:<PORT>. wrk pinned to
# CPU 3 so the server's 4 worker pthreads see CPUs 0-3 (server gets the
# tail latency hit of co-located wrk; that's the real-world apples-to-apples
# shape unless you can afford a separate load-gen box).

set -u
cd "$(dirname "$0")/.."

WORKERS=${HTTP_WORKERS:-4}
DURATION=${WRK_DURATION:-5s}
CONNS=${WRK_CONNS:-200}
THREADS=${WRK_THREADS:-2}
RUNS=${BENCH_RUNS:-3}

kill_servers() {
    pkill -9 -f http_hello_multi 2>/dev/null
    pkill -9 -f http_sm_handler 2>/dev/null
    pkill -9 -f http_sm_hello 2>/dev/null
    pkill -9 -f http_macro_smoke 2>/dev/null
    pkill -9 -f http_router_bench 2>/dev/null
    pkill -9 -f axum_hello 2>/dev/null
    pkill -9 nginx 2>/dev/null
    sleep 1
}

median_of() {
    printf "%s\n" "$@" | sort -n | awk '{ a[NR] = $1 } END { print a[int(NR/2)+1] }'
}

run_one() {
    local label="$1" port="$2" start_cmd="$3"
    echo "=== $label ($WORKERS workers, $DURATION, ${THREADS}t/${CONNS}c, median of $RUNS) ==="
    kill_servers
    eval "$start_cmd > /tmp/bench_srv.log 2>&1 &"
    sleep 1
    if ! curl -sSf --max-time 2 "http://127.0.0.1:$port/" > /dev/null; then
        echo "  -- $label failed to respond"
        cat /tmp/bench_srv.log | tail -5
        kill_servers
        return
    fi
    local rates=()
    for r in $(seq 1 "$RUNS"); do
        local rate
        rate=$(wrk -t "$THREADS" -c "$CONNS" -d "$DURATION" "http://127.0.0.1:$port/" 2>/dev/null \
               | awk '/Requests\/sec/ { print $2 }')
        rates+=("$rate")
    done
    local med
    med=$(median_of "${rates[@]}")
    printf "  median: %12s req/s    runs: %s\n" "$med" "${rates[*]}"
    kill_servers
}

# Each entry: label | port | start command.
run_one "glide-http-listen-workers (SM, rich API)"   8080 "HTTP_WORKERS=$WORKERS ./bench/http_hello_multi.bin"
run_one "glide-router-listen-workers (SM via router)" 8080 "HTTP_WORKERS=$WORKERS ./bench/http_router_bench.bin"
run_one "glide-http-sm-bare (string,string -> string)" 8080 "HTTP_WORKERS=$WORKERS ./bench/http_sm_handler.bin"
run_one "glide-http-sm-hello (hardcoded body)"        8080 "./bench/http_sm_hello.bin"

# Axum reference (Rust + Tokio).
if [ -x ./bench/rust/target/release/axum_hello ]; then
    run_one "axum-tokio" 8080 "HTTP_WORKERS=$WORKERS ./bench/rust/target/release/axum_hello"
fi

# nginx reference.
if [ -r /tmp/nginx_bench.conf ]; then
    run_one "nginx" 8081 "nginx -c /tmp/nginx_bench.conf -p /tmp/"
fi

echo "done."

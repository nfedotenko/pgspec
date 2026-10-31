#!/usr/bin/env bash
# pgspec pipeline: profile -> analyze -> specialize -> verify -> benchmark
set -euo pipefail
cd "$(dirname "$0")"

# Local secrets (e.g. ANTHROPIC_API_KEY for the LLM proposer); gitignored.
[ -f .env ] && { set -a; . ./.env; set +a; }

CXX=${CXX:-clang++}
# Project venv (for the LLM proposer's anthropic SDK) wins over system python.
PY=${PY:-$([ -x .venv/bin/python3 ] && echo .venv/bin/python3 || echo python3)}
# -ffp-contract=off keeps FP math bit-identical between the generic and
# specialized builds, so the differential test can demand exact equality.
FLAGS="-std=c++20 -O2 -ffp-contract=off -Isrc -Igenerated"
ORDERS=${ORDERS:-2000000}
REPS=${REPS:-25}
RUNS=${RUNS:-3}
PROFILE_MODE=${PROFILE_MODE:-multi}
PROPOSER=${PROPOSER:-rule}   # rule | llm (llm needs ANTHROPIC_API_KEY; falls back to rule)
PROFILE_ORDERS=${PROFILE_ORDERS:-1000000}

mkdir -p build

step() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

step "1. profiling build + profile collection ($PROFILE_MODE 'production' replay)"
cp generated/specialized.passthrough.hpp generated/specialized.hpp
$CXX $FLAGS -DVALPROF -o build/engine_prof src/main.cpp
./build/engine_prof --mode "$PROFILE_MODE" --orders "$PROFILE_ORDERS" --reps 1 \
    --profile-out build/profile.json > /dev/null
echo "profile written to build/profile.json"

step "2. analyze profile -> generated/specialized.hpp (proposer: $PROPOSER)"
"$PY" tools/analyze.py build/profile.json generated/specialized.hpp \
    --proposer "$PROPOSER" ${MOCK_RESPONSE:+--mock-response "$MOCK_RESPONSE"} \
    --min-share "${MIN_SHARE:-0.05}" --max-specs "${MAX_SPECS:-8}" \
    --proposal-out build/proposal.json \
    --report generated/report.md

step "3. build baseline (generic) + per-order / per-burst / per-instrument binaries"
$CXX $FLAGS -DFORCE_GENERIC -o build/engine_base src/main.cpp
$CXX $FLAGS -o build/engine_spec src/main.cpp
$CXX $FLAGS -DPGSPEC_BURST -o build/engine_burst src/main.cpp
$CXX $FLAGS -DPGSPEC_INSTR -o build/engine_instr src/main.cpp
echo "built build/engine_base, build/engine_spec, build/engine_burst, build/engine_instr"

run_one() { # binary mode extra_flags -> "checksum ns"
    local out
    # shellcheck disable=SC2086
    out=$("$1" --mode "$2" $3 --orders "$ORDERS" --reps "$REPS")
    echo "$(sed -n 's/.*checksum=\(0x[0-9a-f]*\).*/\1/p' <<< "$out") \
          $(sed -n 's/.*ns_per_order=\([0-9.]*\).*/\1/p' <<< "$out")"
}

step "4. differential test + benchmark (interleaved, min of $RUNS)"
fail=0
printf '\n%-28s %10s %10s %10s %10s %16s\n' "workload" "generic" "order" "burst" "instr" "best"

bench_config() { # label mode extra_flags
    local label=$1 mode=$2 extra=$3
    local nb=1e18 ns=1e18 nu=1e18 ni=1e18 cb cs cu ci c n i
    # Interleave the runs so core scheduling and thermal drift hit all
    # four binaries alike; take the minimum as the contention-free estimate.
    for i in $(seq "$RUNS"); do
        read -r c n <<< "$(run_one ./build/engine_base "$mode" "$extra")"
        cb=$c; nb=$(awk -v a="$n" -v b="$nb" 'BEGIN{print (a+0 < b+0) ? a : b}')
        read -r c n <<< "$(run_one ./build/engine_spec "$mode" "$extra")"
        cs=$c; ns=$(awk -v a="$n" -v b="$ns" 'BEGIN{print (a+0 < b+0) ? a : b}')
        read -r c n <<< "$(run_one ./build/engine_burst "$mode" "$extra")"
        cu=$c; nu=$(awk -v a="$n" -v b="$nu" 'BEGIN{print (a+0 < b+0) ? a : b}')
        read -r c n <<< "$(run_one ./build/engine_instr "$mode" "$extra")"
        ci=$c; ni=$(awk -v a="$n" -v b="$ni" 'BEGIN{print (a+0 < b+0) ? a : b}')
    done
    if [ "$cb" != "$cs" ] || [ "$cb" != "$cu" ] || [ "$cb" != "$ci" ]; then
        echo "DIFF FAIL ($label): base=$cb order=$cs burst=$cu instr=$ci"
        fail=1
        return
    fi
    read -r last_strategy last_speedup <<< "$(awk -v base="$nb" \
        -v o="$ns" -v u="$nu" -v i="$ni" 'BEGIN{
            best="per-order"; n=o;
            if (u+0 < n+0) { best="per-burst"; n=u }
            if (i+0 < n+0) { best="per-instr"; n=i }
            printf "%s %.3f", best, base / n }')"
    printf '%-28s %7s ns %7s ns %7s ns %7s ns %9s %.2fx\n' "$label" \
        "$nb" "$ns" "$nu" "$ni" "$last_strategy" "$last_speedup"
}

bench_config "$PROFILE_MODE / stateful chain" "$PROFILE_MODE" "--chain"
primary_strategy=$last_strategy
primary_speedup=$last_speedup
bench_config "$PROFILE_MODE / throughput"     "$PROFILE_MODE" ""
bench_config "uniform / stateful chain"       uniform         "--chain"
bench_config "uniform / throughput"           uniform         ""

echo
[ "$fail" = 0 ] && echo "differential test: PASS on all 4 workloads x 4 binaries ($ORDERS orders x $REPS reps x $RUNS runs)"
[ "$fail" = 0 ] || exit "$fail"
echo "best strategy on primary workload: $primary_strategy (${primary_speedup}x)"

# A specialization that does not beat the baseline on the primary workload
# must never ship: measurement, not the proposer, has the final word.
step "5. accept/reject gate (primary: $PROFILE_MODE / stateful chain, need >= ${GATE:-1.02}x)"
if awk -v s="$primary_speedup" -v g="${GATE:-1.02}" 'BEGIN{exit !(s >= g)}'; then
    echo "ACCEPTED: specialization kept ($primary_strategy dispatch, ${primary_speedup}x on primary workload)"
else
    cp generated/specialized.passthrough.hpp generated/specialized.hpp
    $CXX $FLAGS -o build/engine_spec src/main.cpp
    echo "REJECTED: ${primary_speedup}x < ${GATE:-1.02}x — reverted to passthrough, engine_spec rebuilt generic"
    exit 0  # nothing to monitor
fi

# A specialization that shipped must notice when production drifts away
# from the profile it was built from: the monitored build counts guard
# hits/misses and flags a live miss-rate far above the profiled one.
step "6. regime monitor (-DPGSPEC_MONITOR build)"
$CXX $FLAGS -DPGSPEC_MONITOR -o build/engine_mon src/main.cpp

mon_line() { # mode -> the MONITOR line
    ./build/engine_mon --mode "$1" --chain --orders "$ORDERS" --reps 3 | grep '^MONITOR'
}
mon_in=$(mon_line "$PROFILE_MODE")
mon_out=$(mon_line uniform)
echo "in-distribution ($PROFILE_MODE): $mon_in"
echo "drifted (uniform):              $mon_out"

d_in=$(sed -n 's/.*drift=\([01]\).*/\1/p' <<< "$mon_in")
d_out=$(sed -n 's/.*drift=\([01]\).*/\1/p' <<< "$mon_out")
if [ "$d_in" = "0" ] && [ "$d_out" = "1" ]; then
    echo "monitor check: PASS (healthy in-distribution, drift flagged on shifted workload)"
else
    echo "monitor check: FAIL (expected drift=0 in-distribution and drift=1 on shifted)"
    fail=1
fi

read -r _ ns_plain <<< "$(run_one ./build/engine_spec "$PROFILE_MODE" "--chain")"
read -r _ ns_mon   <<< "$(run_one ./build/engine_mon  "$PROFILE_MODE" "--chain")"
echo "monitor overhead on primary workload: ${ns_plain} -> ${ns_mon} ns/order"
exit "$fail"

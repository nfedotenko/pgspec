#!/usr/bin/env bash
# pgspec pipeline: profile -> analyze -> specialize -> verify -> benchmark
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
# Project venv (for the LLM proposer's anthropic SDK) wins over system python.
PY=${PY:-$([ -x .venv/bin/python3 ] && echo .venv/bin/python3 || echo python3)}
# -ffp-contract=off keeps FP math bit-identical between the generic and
# specialized builds, so the differential test can demand exact equality.
FLAGS="-std=c++20 -O2 -ffp-contract=off -Isrc -Igenerated"
ORDERS=${ORDERS:-2000000}
REPS=${REPS:-25}
RUNS=${RUNS:-3}
PROFILE_MODE=${PROFILE_MODE:-bimodal}
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
    --proposal-out build/proposal.json \
    --report generated/report.md

step "3. build baseline (generic) and specialized binaries"
$CXX $FLAGS -DFORCE_GENERIC -o build/engine_base src/main.cpp
$CXX $FLAGS -o build/engine_spec src/main.cpp
echo "built build/engine_base and build/engine_spec"

run_one() { # binary mode extra_flags -> "checksum ns"
    local out
    # shellcheck disable=SC2086
    out=$("$1" --mode "$2" $3 --orders "$ORDERS" --reps "$REPS")
    echo "$(sed -n 's/.*checksum=\(0x[0-9a-f]*\).*/\1/p' <<< "$out") \
          $(sed -n 's/.*ns_per_order=\([0-9.]*\).*/\1/p' <<< "$out")"
}

step "4. differential test + benchmark (interleaved, min of $RUNS)"
fail=0
printf '\n%-30s %12s %12s %10s\n' "workload" "generic" "specialized" "speedup"

bench_config() { # label mode extra_flags
    local label=$1 mode=$2 extra=$3
    local nb=1e18 ns=1e18 cb cs c n i
    # Interleave base/spec runs so core scheduling and thermal drift hit
    # both binaries alike; take the minimum as the contention-free estimate.
    for i in $(seq "$RUNS"); do
        read -r c n <<< "$(run_one ./build/engine_base "$mode" "$extra")"
        cb=$c; nb=$(awk -v a="$n" -v b="$nb" 'BEGIN{print (a+0 < b+0) ? a : b}')
        read -r c n <<< "$(run_one ./build/engine_spec "$mode" "$extra")"
        cs=$c; ns=$(awk -v a="$n" -v b="$ns" 'BEGIN{print (a+0 < b+0) ? a : b}')
    done
    if [ "$cb" != "$cs" ]; then
        echo "DIFF FAIL ($label): $cb != $cs"
        fail=1
        return
    fi
    last_speedup=$(awk -v a="$nb" -v b="$ns" 'BEGIN{printf "%.3f", a / b}')
    printf '%-30s %10s ns %10s ns %9sx\n' "$label" "$nb" "$ns" \
        "$(awk -v s="$last_speedup" 'BEGIN{printf "%.2f", s}')"
}

bench_config "$PROFILE_MODE / stateful chain" "$PROFILE_MODE" "--chain"
primary_speedup=$last_speedup
bench_config "$PROFILE_MODE / throughput"     "$PROFILE_MODE" ""
bench_config "uniform / stateful chain"       uniform         "--chain"
bench_config "uniform / throughput"           uniform         ""

echo
[ "$fail" = 0 ] && echo "differential test: PASS on all 4 workloads ($ORDERS orders x $REPS reps x $RUNS runs)"
[ "$fail" = 0 ] || exit "$fail"

# A specialization that does not beat the baseline on the primary workload
# must never ship: measurement, not the proposer, has the final word.
step "5. accept/reject gate (primary: $PROFILE_MODE / stateful chain, need >= ${GATE:-1.02}x)"
if awk -v s="$primary_speedup" -v g="${GATE:-1.02}" 'BEGIN{exit !(s >= g)}'; then
    echo "ACCEPTED: specialization kept (${primary_speedup}x on primary workload)"
else
    cp generated/specialized.passthrough.hpp generated/specialized.hpp
    $CXX $FLAGS -o build/engine_spec src/main.cpp
    echo "REJECTED: ${primary_speedup}x < ${GATE:-1.02}x — reverted to passthrough, engine_spec rebuilt generic"
fi

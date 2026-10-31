# pgspec — profile-guided specialization MVP

An offline-JIT experiment: run a binary on a recorded workload, observe
which *values* dominate at runtime, then regenerate the build so those
values become compile-time constants on the hot path — guarded, with a
generic fallback. Think "what a tracing JIT does", but ahead-of-time.

## Pipeline

```
+-------------------+     profile.json      +------------------+
| engine (-DVALPROF)| --------------------> | tools/analyze.py |
| value probes on   |   per-site value      | rule-based       |
| the hot path      |   histograms          | proposer+codegen |
+-------------------+                       +--------+---------+
                                                     | generated/specialized.hpp
                                                     v
                         +--------------------------------------------+
                         | rebuild:                                   |
                         |   engine_base  (generic, FORCE_GENERIC)    |
                         |   engine_spec  (guard -> Spec0 constants)  |
                         +---------------------+----------------------+
                                               v
                          differential test (bit-exact checksums)
                          + benchmark (skew & adversarial uniform)
```

Run it:

```bash
./pipeline.sh                                # rule-based proposer
PROPOSER=llm ./pipeline.sh                   # LLM proposer (needs ANTHROPIC_API_KEY)
PROPOSER=llm MOCK_RESPONSE=tools/example_proposal.json ./pipeline.sh  # offline test
```

## Proposers

Two proposers share one spec contract (guard terms + per-site accessor
expressions) and one codegen path in `tools/analyze.py`:

- **rule** (default): top-k joint value tuples above `--min-share`, every
  field pinned to a constant.
- **llm** (`tools/llm_proposer.py`): Claude (`claude-opus-5-5` by default,
  `--model` to override) receives the hot-path source and the profile and
  returns specs as structured JSON (`output_config.format`, so parsing
  never needs repair). It can propose what the rule can't — e.g. *partial*
  specialization: pin only `type` for the 99%-IOC tail, keep lot/tick as
  identity reads. Setup: `python3 -m venv .venv && .venv/bin/pip install
  anthropic`, export `ANTHROPIC_API_KEY` (the pipeline prefers `.venv`
  automatically).

Trust boundary: the LLM only *proposes*. Each returned expression must
tokenize against a strict whitelist (fields of `o`, `OrderType::` enums,
numeric literals, `std::bit_cast<double>(0x...ULL)`, comparison/arithmetic
operators — anything else is rejected before codegen); whatever passes is
then compiled, bit-exactly differential-tested and benchmark-gated like any
other spec. Any failure — no key, network, refusal, invalid JSON, rejected
expression — degrades to the rule proposer with a warning. The raw proposal
is saved to `build/proposal.json` for inspection.

## What gets specialized

The toy hot path (`src/engine.hpp`) is a policy template
`process_order<Spec>`. `GenericSpec` reads every parameter from the
order at runtime. The analyzer ranks the **joint distribution of value
tuples** at the dispatch site (marginals are not enough: two fields can
each look bimodal while only two of their four combinations ever
co-occur, and a guard is a conjunction) and emits one `SpecN` struct per
tuple above `--min-share`, up to `--max-specs`. Each spec returns its
tuple's values as `constexpr`, so after inlining:

- `qty / lot`, `qty % lot`, `price / lot` — integer division by a
  runtime value becomes division by a constant (magic-number multiply);
- the fee `switch` on order type folds away;
- the tick-size multiply becomes a constant multiply.

The generated `SpecList` is tried hottest-first by a compile-time
dispatch chain (`dispatch_specs` in `engine.hpp`); the first guard that
holds wins, otherwise the generic instantiation runs. On the bimodal
workload (two hot instruments traded in bursts) two specs cover ~95% of
orders and beat both the baseline and the single-spec build. Each guard
is emitted **branchless** (every
compare materializes as a flag, combined with bitwise `&`, one final
heavily-biased branch) — a short-circuit `&&` chain compiled to three
data-dependent branches and cost ~14% on workloads that drift from the
profile; the branchless form brought that to parity. The adversarial
`uniform` runs in the pipeline measure exactly that fallback cost.

## Measured findings (Apple M-series)

- **Throughput-bound loops punish this specialization** (0.74x): with
  independent iterations the OoO core hides `udiv` latency in the
  otherwise-idle divider unit, while the magic-multiply replacement
  competes for shared ALU ports and the guard adds work on top.
- **Latency-bound (stateful) processing rewards it** (~1.17x): the
  `--chain` mode feeds the running position back into the next order's
  size — as real engines do via inventory/risk — putting the division on
  a serial dependency chain where its latency can't be hidden.
- **The benchmark is part of the product.** Two measurement artifacts
  produced wrong verdicts before the real one: a serial FNV checksum
  chain that hid the hot path entirely, and macOS core scheduling
  (P- vs E-core) that made identical binaries differ by 1.9x between
  runs. Hence independent accumulators, interleaved runs, min-of-N.
- **Generated code can push the surrounding code past inliner
  thresholds.** With two specs the hot-loop lambda stopped being
  inlined, forcing the running position through a store-load on the
  serial chain — which looked exactly like "multi-spec doesn't pay"
  (0.84x) until the disassembly showed the outlined call. Forced
  inlining restored 1.17x with specs beating the single-spec build.
  A production tool must watch inlining decisions around its codegen.
- **Partial specialization pays on mixed workloads.** A proposed spec that
  pins only `type == Ioc` (lot/tick stay runtime reads) made the
  *adversarial uniform* workload 1.4-1.6x faster while keeping the primary
  bimodal win: the guard acts as a predictable pre-classifier, splitting
  one unpredictable 4-way switch into two far more biased paths. The rule
  proposer cannot express this; the proposer seam can.
- Consequence of all of the above: the pipeline ends with an
  **accept/reject gate** — a specialization that does not beat baseline
  on the primary workload is reverted. The proposer suggests;
  measurement decides.

## Correctness contract

The specialized build must produce **bit-identical** results to the
generic one on any input (guards select *which code path*, never *what
it computes*). The pipeline enforces this with FNV checksums over all
results on both the profiled-like and the adversarial workload, and the
builds use `-ffp-contract=off` so FP folding stays IEEE-identical.

## Where this goes next (not in the MVP)

- **Probe autogeneration** — probes and the analyzer's site registry are
  hand-written here; C++26 reflection (P2996 + annotations) over marked
  structs should emit both, and generate Spec structs/SoA variants
  directly instead of an external header generator.
- **Richer LLM proposals** — the proposer currently emits specs within the
  guard/accessor contract; the next step is letting it propose structural
  transforms (SoA splits, flat maps for dense keys, memoization) through a
  wider codegen vocabulary, still gated by the same differential test.
- **Non-intrusive collection** — replace -DVALPROF instrumentation with
  perf/LBR/Intel PT sampling on a shadow instance for production use.
- **Per-instrument specialization** — key specs by instrument id rather
  than guarding raw field values, with a per-instrument dispatch table.
- **Regime monitoring** — track guard miss-rate in production; a rising
  miss-rate (market regime change) should trigger re-profiling, and the
  fallback path must stay fast, not merely correct.

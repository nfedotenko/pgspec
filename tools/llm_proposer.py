#!/usr/bin/env python3
"""LLM proposer for pgspec: sends the hot-path C++ source and the runtime
value profile to Claude and returns proposed specialization specs as JSON.

The proposer only PROPOSES. Everything it returns is (1) validated against
a token whitelist, (2) compiled, (3) bit-exactly differential-tested against
the generic build, and (4) benchmark-gated before it can ship. A wrong or
harmful proposal is rejected by the pipeline, never silently kept.
"""

import json

MODEL_DEFAULT = "claude-opus-5-5"

# Structured-output schema: the API guarantees the response text is valid
# JSON matching this shape, so parsing never needs repair heuristics.
SPEC_SCHEMA = {
    "type": "object",
    "properties": {
        "specs": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "rationale": {"type": "string"},
                    "guard_terms": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                    "accessors": {
                        "type": "object",
                        "properties": {
                            "type": {"type": "string"},
                            "lot_size": {"type": "string"},
                            "tick_size": {"type": "string"},
                        },
                        "required": ["type", "lot_size", "tick_size"],
                        "additionalProperties": False,
                    },
                },
                "required": ["name", "rationale", "guard_terms", "accessors"],
                "additionalProperties": False,
            },
        },
        "notes": {"type": "string"},
    },
    "required": ["specs", "notes"],
    "additionalProperties": False,
}

SYSTEM = """You are the proposer inside pgspec, a profile-guided \
specialization tool for C++ trading-system hot paths. You receive the hot \
path's source and a runtime value profile, and you propose guarded \
compile-time specializations. You are rewarded for specs that survive the \
pipeline's bit-exact differential test AND beat the generic baseline on a \
latency-bound benchmark; a rejected spec is wasted work, so propose only \
what you can justify from the profile."""

CONTRACT = """\
Propose up to {max_specs} specialization specs for the hot path below. For
each spec N the pipeline generates a guarded fast path at the dispatch site:

    if (SpecN::guard(o)) return process_order(o, SpecN{{}});

where SpecN's accessors replace the raw order-field reads inside
process_order<Spec>.

Rules:
- guard_terms: C++ boolean expressions over `const Order& o`. They are
  AND-ed together and compiled branchless (one biased branch at the call
  site). Keep them cheap.
- accessors: C++ expressions for the values of type / lot_size / tick_size
  that process_order will use. Return a LITERAL CONSTANT wherever the guard
  pins the field's value — constant folding (division by a constant,
  dead-switch elimination) is the entire benefit. If a spec does not pin a
  field, return the identity read (e.g. `o.tick_size`).
- CORRECTNESS CONTRACT: whenever ALL guard_terms hold for an order, every
  accessor expression MUST equal the corresponding field of that order
  exactly. The pipeline runs a bit-exact differential test against the
  generic build; any divergence rejects the proposal.
- Write double literals as std::bit_cast<double>(0x...ULL) using the exact
  bit patterns given in the profile.
- Allowed tokens only: o.type, o.lot_size, o.tick_size, o.qty,
  o.price_ticks, OrderType::Limit / Market / Ioc / Fok,
  std::bit_cast<double>(0x...ULL), integer and hex literals with u/ULL
  suffixes, true, false, operators == != < <= > >= && || & | ! + - * / %
  and parentheses. Anything else fails validation.
- Specs are tried in the order you return them; the first guard that holds
  wins. Order them hottest-first. Every spec adds code size and a branch at
  the dispatch site, so prefer FEW specs with HIGH coverage; a spec with a
  tiny expected hit-rate is a net loss.
- Measured reality in this project (Apple Silicon): integer division by a
  runtime value is nearly free in throughput-bound loops but expensive on
  serial dependency chains; the PRIMARY benchmark is latency-bound
  (a stateful position-feedback chain). Branch-predictor behaviour matters:
  the workload arrives in per-instrument bursts.
- In "notes", put observations for the human operator: expected coverage,
  risks, and what you would try next beyond value specialization.

== Hot path source ==

{sources}

== Runtime value profile ==

{profile}
"""


def propose(sources: str, profile_text: str, max_specs: int,
            model: str = MODEL_DEFAULT, mock_response: str | None = None) -> dict:
    """Return the raw proposal dict {"specs": [...], "notes": "..."}."""
    if mock_response:
        with open(mock_response) as f:
            return json.load(f)

    import anthropic  # lazy: the rule proposer must work without the SDK

    client = anthropic.Anthropic()
    response = client.messages.create(
        model=model,
        max_tokens=16000,
        system=SYSTEM,
        messages=[{
            "role": "user",
            "content": CONTRACT.format(
                max_specs=max_specs, sources=sources, profile=profile_text),
        }],
        output_config={"format": {"type": "json_schema", "schema": SPEC_SCHEMA}},
    )
    if response.stop_reason == "refusal":
        raise RuntimeError("model refused the request "
                           f"({response.stop_details and response.stop_details.category})")
    text = next(b.text for b in response.content if b.type == "text")
    return json.loads(text)

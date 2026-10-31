#pragma once
#include <tuple>

#include "types.hpp"
#include "monitor.hpp"   // before specialized.hpp: generated code uses its macros
#include "specialized.hpp"

#ifdef VALPROF
#include "valprof.hpp"
#endif

// Generic spec: every parameter is a runtime load from the order.
// A specialized spec (generated/specialized.hpp) returns compile-time
// constants instead, so divisions, the fee switch and the tick multiply
// all fold after inlining.
struct GenericSpec {
    static constexpr bool specialized = false;
    OrderType type(const Order& o) const { return o.type; }
    uint32_t  lot_size(const Order& o) const { return o.lot_size; }
    double    tick_size(const Order& o) const { return o.tick_size; }
};

template <class Spec>
[[gnu::always_inline]] inline Result process_order(const Order& o, Spec spec) {
    const uint32_t  lot  = spec.lot_size(o);
    const double    tick = spec.tick_size(o);
    const OrderType ty   = spec.type(o);

    // With constexpr lot these become magic-number multiplies; with a
    // runtime lot they are real integer divisions.
    const uint32_t lots = o.qty / lot;
    const uint32_t odd  = o.qty % lot;
    const int64_t  price_per_lot = o.price_ticks / (int64_t)lot;

    const int64_t filled   = (int64_t)lots * (int64_t)lot;
    const double  notional = (double)o.price_ticks * tick * (double)filled;

    double fee_bps;
    switch (ty) {
        case OrderType::Limit:  fee_bps = (price_per_lot > 5000) ? 0.8 : 1.1; break;
        case OrderType::Market: fee_bps = 2.4; break;
        case OrderType::Ioc:    fee_bps = (odd != 0) ? 1.9 : 1.6; break;
        case OrderType::Fok:    fee_bps = 2.1; break;
        default:                fee_bps = 3.0; break;
    }

    Result r;
    r.lots           = lots;
    r.odd_qty        = odd;
    r.notional_milli = (int64_t)(notional * 1000.0);
    r.fee_micro      = (int64_t)(notional * fee_bps * 100.0);
    return r;
}

// The caller supplies a sink(order, spec) that owns per-order state updates
// and result consumption; the dispatchers below only pick the spec. Early
// returns, not a fold with a result merge, so the compiler doesn't build a
// phi web over Result.

// Per-order dispatch: tries each generated spec in SpecList order (hottest
// first, as ranked by the analyzer); the first guard that holds wins.
template <class SpecTuple, size_t I = 0, class Sink>
[[gnu::always_inline]] inline void dispatch_one(const Order& o, Sink&& sink) {
    if constexpr (I < std::tuple_size_v<SpecTuple>) {
        using S = std::tuple_element_t<I, SpecTuple>;
        if (S::guard(o)) {
            PGSPEC_MON_HIT(I);
            sink(o, S{});
            return;
        }
        dispatch_one<SpecTuple, I + 1>(o, sink);
    } else {
        PGSPEC_MON_GENERIC();
        sink(o, GenericSpec{});
    }
}

template <class SpecTuple, size_t I = 0>
[[gnu::always_inline]] inline bool any_guard(const Order& o) {
    if constexpr (I < std::tuple_size_v<SpecTuple>) {
        return std::tuple_element_t<I, SpecTuple>::guard(o) ||
               any_guard<SpecTuple, I + 1>(o);
    } else {
        return false;
    }
}

// Per-burst dispatch: orders arrive in per-instrument bursts, so instead of
// re-running the guard cascade per order, classify the head of a run and
// process the whole run through ONE monomorphic loop — the guard survives
// only as that loop's exit condition (one predictable check per order, and
// the cascade cost is paid once per run, not once per order).
template <class SpecTuple, size_t I = 0, class Sink>
[[gnu::always_inline]] inline size_t dispatch_run(const Order* p, size_t n,
                                                  Sink&& sink) {
    if constexpr (I < std::tuple_size_v<SpecTuple>) {
        using S = std::tuple_element_t<I, SpecTuple>;
        if (S::guard(p[0])) {
            size_t i = 0;
            do {
                PGSPEC_MON_HIT(I);
                sink(p[i], S{});
                ++i;
            } while (i < n && S::guard(p[i]));
            return i;
        }
        return dispatch_run<SpecTuple, I + 1>(p, n, sink);
    } else {
        size_t i = 0;
        do {
            PGSPEC_MON_GENERIC();
            sink(p[i], GenericSpec{});
            ++i;
        } while (i < n && !any_guard<SpecTuple>(p[i]));
        return i;
    }
}

template <class SpecTuple, class Sink>
inline void process_stream(const Order* p, size_t n, Sink&& sink) {
    size_t i = 0;
    while (i < n) i += dispatch_run<SpecTuple>(p + i, n - i, sink);
}

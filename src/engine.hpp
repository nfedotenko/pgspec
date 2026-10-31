#pragma once
#include <tuple>

#include "types.hpp"
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

// Tries each generated spec in SpecList order (hottest first, as ranked by
// the analyzer); the first guard that holds wins, otherwise generic.
// Early returns, not a fold with a result merge: each path tail-returns its
// own instantiation, so the compiler doesn't build a phi web over Result.
template <class SpecTuple, size_t I = 0>
[[gnu::always_inline]] inline Result dispatch_specs(const Order& o) {
    if constexpr (I < std::tuple_size_v<SpecTuple>) {
        using S = std::tuple_element_t<I, SpecTuple>;
        if (S::guard(o)) return process_order(o, S{});
        return dispatch_specs<SpecTuple, I + 1>(o);
    } else {
        return process_order(o, GenericSpec{});
    }
}

[[gnu::always_inline]] inline Result process_dispatch(const Order& o) {
#ifdef VALPROF
    VP_U64("order.type:enum", (uint64_t)o.type);
    VP_U64("order.lot_size:u32", o.lot_size);
    VP_F64("order.tick_size:f64", o.tick_size);
    VP_JOINT3("order.hot:joint", (uint64_t)o.type, o.lot_size,
              std::bit_cast<uint64_t>(o.tick_size));
#endif
#if PGSPEC_ENABLED && !defined(FORCE_GENERIC)
    return dispatch_specs<pgspec::SpecList>(o);
#else
    return process_order(o, GenericSpec{});
#endif
}

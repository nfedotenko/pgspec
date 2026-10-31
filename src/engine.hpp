#pragma once
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

[[gnu::always_inline]] inline Result process_dispatch(const Order& o) {
#ifdef VALPROF
    VP_U64("order.type:enum", (uint64_t)o.type);
    VP_U64("order.lot_size:u32", o.lot_size);
    VP_F64("order.tick_size:f64", o.tick_size);
#endif
#if PGSPEC_ENABLED && !defined(FORCE_GENERIC)
    if (pgspec::Spec0::guard(o)) [[likely]]
        return process_order(o, pgspec::Spec0{});
#endif
    return process_order(o, GenericSpec{});
}

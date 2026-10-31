#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "engine.hpp"

namespace {

enum class GenMode { Skew, Uniform, Bimodal };

std::vector<Order> gen_orders(size_t n, GenMode mode, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>     d1000(0, 999);
    std::uniform_int_distribution<uint32_t> dqty(1, 10000);
    std::uniform_int_distribution<int64_t>  dprice(100, 1000000);

    static constexpr uint32_t kLots[]  = {10, 50, 200};
    static constexpr double   kTicks[] = {0.05, 0.25};

    std::vector<Order> v;
    v.reserve(n);
    int  burst_left = 0;
    bool hot_b      = false;
    for (size_t i = 0; i < n; ++i) {
        Order o;
        const int r1 = d1000(rng), r2 = d1000(rng), r3 = d1000(rng);
        switch (mode) {
        case GenMode::Skew:
            // The "production" distribution: one dominant value per field.
            o.type      = (r1 < 990) ? OrderType::Ioc : OrderType(r1 % 3 == 2 ? 3 : r1 % 3);
            o.lot_size  = (r2 < 970) ? 100u : kLots[r2 % 3];
            o.tick_size = (r3 < 980) ? 0.01 : kTicks[r3 % 2];
            break;
        case GenMode::Uniform:
            // Adversarial: no dominant value, guards mostly miss.
            o.type      = OrderType(r1 % 4);
            o.lot_size  = (r2 % 4 == 0) ? 100u : kLots[r2 % 3];
            o.tick_size = (r3 % 3 == 0) ? 0.01 : kTicks[r3 % 2];
            break;
        case GenMode::Bimodal:
            // Two hot instruments with different lot/tick profiles, traded
            // in bursts (as real feeds are), plus ~5% background noise.
            // Marginals look mixed; the joint distribution has two modes.
            if (burst_left == 0) {
                hot_b      = (r1 >= 550);        // ~55% mode A / ~45% mode B
                burst_left = 16 + (r2 % 113);    // burst of 16..128 orders
            }
            --burst_left;
            if (r3 < 50) {
                o.type      = OrderType(r1 % 4);
                o.lot_size  = kLots[r2 % 3];
                o.tick_size = kTicks[r1 % 2];
            } else if (hot_b) {
                o.type = OrderType::Ioc; o.lot_size = 10u;  o.tick_size = 0.25;
            } else {
                o.type = OrderType::Ioc; o.lot_size = 100u; o.tick_size = 0.01;
            }
            break;
        }
        o.qty         = dqty(rng);
        o.price_ticks = dprice(rng);
        v.push_back(o);
    }
    return v;
}

// Chain=false: pure throughput — iterations are independent, the OoO core
// overlaps them, so per-order cost is bound by execution resources.
// Chain=true: stateful processing — running position feeds the next order's
// size (inventory-dependent sizing, as real engines do), which puts the
// qty -> division -> position dependency on a serial chain, so per-order
// cost is bound by the LATENCY of that chain.
template <bool Chain>
double run_loop(const std::vector<Order>& orders, int reps, uint64_t* checksum) {
    using clock = std::chrono::steady_clock;
    // Independent accumulators: deterministic (sum/xor are commutative) but
    // no serial dependency chain through the checksum itself.
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int64_t position = 0;

    // always_inline: with several generated specs the body outgrows the
    // inliner's threshold, and an outlined lambda forces position and the
    // accumulators through memory on every order — the store-load of
    // position lands straight on the serial dependency chain.
    auto step = [&](const Order& o) __attribute__((always_inline)) {
        Order w = o;
        if constexpr (Chain) w.qty += (uint32_t)((uint64_t)position & 0x3fULL);
        Result res = process_dispatch(w);
        if constexpr (Chain)
            position += (res.odd_qty & 1u) ? (int64_t)res.lots : -(int64_t)res.lots;
        a0 += res.lots; a1 ^= res.odd_qty;
        a2 += (uint64_t)res.notional_milli; a3 ^= (uint64_t)res.fee_micro;
    };

    // Warmup rep (untimed).
    for (const Order& o : orders) step(o);

    const auto t0 = clock::now();
    for (int r = 0; r < reps; ++r)
        for (const Order& o : orders) step(o);
    const auto t1 = clock::now();

    uint64_t c = 0xcbf29ce484222325ULL;
    for (uint64_t a : {a0, a1, a2, a3}) c = (c ^ a) * 0x100000001b3ULL;
    *checksum = c;
    const double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return ns / ((double)orders.size() * (double)reps);
}

}  // namespace

int main(int argc, char** argv) {
    size_t      orders = 1000000;
    int         reps   = 10;
    GenMode     mode   = GenMode::Skew;
    const char* mode_s = "skew";
    bool        chain  = false;
    uint64_t    seed   = 42;
    const char* profile_out = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--orders") && i + 1 < argc)      orders = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc)   reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)   seed = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode_s = argv[++i];
            if      (!strcmp(mode_s, "skew"))    mode = GenMode::Skew;
            else if (!strcmp(mode_s, "uniform")) mode = GenMode::Uniform;
            else if (!strcmp(mode_s, "bimodal")) mode = GenMode::Bimodal;
            else { fprintf(stderr, "unknown mode: %s\n", mode_s); return 2; }
        }
        else if (!strcmp(argv[i], "--chain"))                  chain = true;
        else if (!strcmp(argv[i], "--profile-out") && i + 1 < argc) profile_out = argv[++i];
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    const std::vector<Order> data = gen_orders(orders, mode, seed);

    uint64_t checksum = 0;
    const double ns_per_order = chain ? run_loop<true>(data, reps, &checksum)
                                      : run_loop<false>(data, reps, &checksum);

#ifdef VALPROF
    if (profile_out) valprof::Registry::get().dump(profile_out);
#else
    (void)profile_out;
#endif

    printf("RESULT checksum=0x%016" PRIx64 " ns_per_order=%.3f mode=%s chain=%d spec=%s\n",
           checksum, ns_per_order, mode_s, chain ? 1 : 0,
#if PGSPEC_ENABLED && !defined(FORCE_GENERIC)
           "on"
#else
           "off"
#endif
    );
    return 0;
}

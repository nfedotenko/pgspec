#pragma once
// Guard miss-rate monitor: per-spec hit counters plus a generic-fallback
// counter, compiled in only under -DPGSPEC_MONITOR. The live miss-rate is
// compared against the expectation computed from the profile at codegen
// time (pgspec::kExpectedMiss); a sustained excess means the production
// distribution has drifted from the profile and the binary should be
// re-profiled — exactly the "pattern breaks on the volatile day" risk.
#include <cstdint>

namespace pgspec_mon {

struct Counters {
    static constexpr int kMaxSpecs = 8;
    uint64_t hits[kMaxSpecs] = {};
    uint64_t generic = 0;
};

inline Counters g;

}  // namespace pgspec_mon

#ifdef PGSPEC_MONITOR
#define PGSPEC_MON_HIT(I) (++::pgspec_mon::g.hits[(I)])
#define PGSPEC_MON_GENERIC() (++::pgspec_mon::g.generic)
#else
#define PGSPEC_MON_HIT(I) ((void)0)
#define PGSPEC_MON_GENERIC() ((void)0)
#endif

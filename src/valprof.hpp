#pragma once
// Value profiler: per-site histograms of observed values, dumped as JSON.
// Compiled in only under -DVALPROF; the release hot path never sees this.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

namespace valprof {

using Hist = std::unordered_map<uint64_t, uint64_t>;

// Joint histogram over a value TUPLE at one site: marginals can't tell
// which combinations actually co-occur, and specialization guards are
// conjunctions, so the joint distribution is what decides coverage.
struct Key3 {
    uint64_t a, b, c;
    bool operator==(const Key3&) const = default;
};
struct Key3Hash {
    size_t operator()(const Key3& k) const {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint64_t v : {k.a, k.b, k.c}) { h ^= v; h *= 0x100000001b3ULL; }
        return (size_t)h;
    }
};
using Hist3 = std::unordered_map<Key3, uint64_t, Key3Hash>;

struct Registry {
    std::map<std::string, Hist> sites;
    std::map<std::string, Hist3> joint_sites;

    static Registry& get() {
        static Registry r;
        return r;
    }

    Hist*  site(const char* name) { return &sites[name]; }
    Hist3* joint(const char* name) { return &joint_sites[name]; }

    void dump(const char* path) {
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "{\n");
        bool first_site = true;
        for (const auto& [name, hist] : sites) {
            if (!first_site) std::fprintf(f, ",\n");
            first_site = false;
            std::fprintf(f, "  \"%s\": {", name.c_str());
            bool first = true;
            for (const auto& [v, c] : hist) {
                if (!first) std::fprintf(f, ", ");
                first = false;
                std::fprintf(f, "\"%llu\": %llu",
                             (unsigned long long)v, (unsigned long long)c);
            }
            std::fprintf(f, "}");
        }
        for (const auto& [name, hist] : joint_sites) {
            if (!first_site) std::fprintf(f, ",\n");
            first_site = false;
            std::fprintf(f, "  \"%s\": {", name.c_str());
            bool first = true;
            for (const auto& [k, c] : hist) {
                if (!first) std::fprintf(f, ", ");
                first = false;
                std::fprintf(f, "\"%llu|%llu|%llu\": %llu",
                             (unsigned long long)k.a, (unsigned long long)k.b,
                             (unsigned long long)k.c, (unsigned long long)c);
            }
            std::fprintf(f, "}");
        }
        std::fprintf(f, "\n}\n");
        std::fclose(f);
    }
};

}  // namespace valprof

// Each macro caches its histogram pointer so the hot profiling loop
// does one hash lookup per sample, not a string-keyed map walk.
#define VP_U64(site_name, v)                                              \
    do {                                                                  \
        static auto* _vp_h = ::valprof::Registry::get().site(site_name);  \
        ++(*_vp_h)[(uint64_t)(v)];                                        \
    } while (0)

#define VP_F64(site_name, v)                                              \
    do {                                                                  \
        static auto* _vp_h = ::valprof::Registry::get().site(site_name);  \
        ++(*_vp_h)[std::bit_cast<uint64_t>((double)(v))];                 \
    } while (0)

#define VP_JOINT3(site_name, va, vb, vc)                                  \
    do {                                                                  \
        static auto* _vp_h = ::valprof::Registry::get().joint(site_name); \
        ++(*_vp_h)[{(uint64_t)(va), (uint64_t)(vb), (uint64_t)(vc)}];     \
    } while (0)

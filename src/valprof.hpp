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

struct Registry {
    std::map<std::string, Hist> sites;

    static Registry& get() {
        static Registry r;
        return r;
    }

    Hist* site(const char* name) { return &sites[name]; }

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

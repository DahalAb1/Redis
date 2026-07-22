// resize_bench.cpp
// In-process microbenchmark isolating ONE thing: what happens to per-insert
// latency while the hashtable grows past its resize thresholds.
//
// Compares:
//   custom   -- the server's HMap (08_hashtables.cpp): incremental resize,
//               migrates <=128 nodes per op, so no single op copies the table.
//   std      -- std::unordered_map: rehashes ALL elements in one op when the
//               load factor trips, so one insert stalls copying ~N nodes.
//
// We time EVERY insert individually and report the tail (p99/p999/max) plus a
// bucketed timeline so the spikes are visible. No network, no server: this
// measures the data structure alone.
//
// Fairness notes (see notes.md):
//  - custom Entry objects + their hashes are pre-built OUTSIDE the timed region,
//    so the custom timing is pure insert+resize work, not malloc.
//  - std::unordered_map is NOT reserve()'d, because a KV server cannot know the
//    final key count in advance. Natural incremental rehashing is the realistic
//    behavior being compared against.
//  - unordered_map::insert also does a dedup lookup that hm_insert does not.
//    That affects the AVERAGE slightly; it does not create the rehash SPIKE,
//    which is the finding.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include "../08_hashtables/08_hashtable.h"

// ---- glue copied from the server so we exercise the real HMap path ----
#define container_of(ptr, type, member) ({                  \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type, member) );})

struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
};

static uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) h = (h + data[i]) * 0x01000193;
    return h;
}

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct Stats { double avg, p50, p99, p999, max; size_t max_at; };

static Stats summarize(std::vector<double> &us) {
    Stats s{};
    double sum = 0;
    s.max = 0; s.max_at = 0;
    for (size_t i = 0; i < us.size(); i++) {
        sum += us[i];
        if (us[i] > s.max) { s.max = us[i]; s.max_at = i; }
    }
    s.avg = sum / us.size();
    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p){ return sorted[(size_t)(p*(sorted.size()-1))]; };
    s.p50 = pct(0.50); s.p99 = pct(0.99); s.p999 = pct(0.999);
    return s;
}

// bucketed max-latency timeline as an ASCII bar chart (log-ish scale)
static void timeline(const char *name, std::vector<double> &us, int nb) {
    size_t n = us.size();
    std::vector<double> bmax(nb, 0);
    for (size_t i = 0; i < n; i++) {
        int b = (int)((double)i / n * nb);
        if (b >= nb) b = nb - 1;
        if (us[i] > bmax[b]) bmax[b] = us[i];
    }
    double gmax = 0;
    for (double v : bmax) gmax = std::max(gmax, v);
    printf("\n%s  (per-bucket MAX latency, bucket = %zu inserts, full bar = %.1f us)\n",
           name, n / nb, gmax);
    for (int b = 0; b < nb; b++) {
        int w = gmax > 0 ? (int)(bmax[b] / gmax * 50 + 0.5) : 0;
        printf("%3d%% |", (b * 100) / nb);
        for (int i = 0; i < w; i++) putchar('#');
        printf(" %.1f\n", bmax[b]);
    }
}

int main(int argc, char **argv) {
    size_t N = argc > 1 ? (size_t)atol(argv[1]) : 1000000;
    int NB   = argc > 2 ? atoi(argv[2]) : 40;

    // pre-generate keys/vals
    std::vector<std::string> keys(N), vals(N);
    for (size_t i = 0; i < N; i++) {
        keys[i] = "key:" + std::to_string(i);
        vals[i] = "val:" + std::to_string(i);
    }

    // ---------- custom HMap ----------
    // Pre-build Entry nodes + hashes outside timing so we measure resize, not malloc.
    std::vector<Entry*> ents(N);
    for (size_t i = 0; i < N; i++) {
        Entry *e = new Entry();
        e->key = keys[i];
        e->val = vals[i];
        e->node.hcode = str_hash((uint8_t*)e->key.data(), e->key.size());
        ents[i] = e;
    }
    HMap db{};
    std::vector<double> lat_custom(N);
    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        hm_insert(&db, &ents[i]->node);
        lat_custom[i] = (now_ns() - t0) / 1000.0; // us
    }

    // ---------- std::unordered_map ----------
    std::unordered_map<std::string, std::string> um;
    std::vector<double> lat_std(N);
    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        um.emplace(keys[i], vals[i]);
        lat_std[i] = (now_ns() - t0) / 1000.0; // us
    }

    Stats c = summarize(lat_custom);
    Stats s = summarize(lat_std);

    printf("Inserted %zu keys. Per-insert latency (microseconds):\n\n", N);
    printf("%-22s %10s %10s %10s %10s %12s\n",
           "structure", "avg", "p50", "p99", "p999", "max");
    printf("%-22s %10.3f %10.3f %10.3f %10.3f %12.1f\n",
           "custom HMap", c.avg, c.p50, c.p99, c.p999, c.max);
    printf("%-22s %10.3f %10.3f %10.3f %10.3f %12.1f\n",
           "std::unordered_map", s.avg, s.p50, s.p99, s.p999, s.max);

    printf("\nmax single-insert stall:\n");
    printf("  custom HMap        : %10.1f us  (at insert #%zu)\n", c.max, c.max_at);
    printf("  std::unordered_map : %10.1f us  (at insert #%zu)\n", s.max, s.max_at);
    printf("  ratio (std / custom worst-case) : %.0fx\n", s.max / c.max);

    timeline("custom HMap", lat_custom, NB);
    timeline("std::unordered_map", lat_std, NB);
    return 0;
}

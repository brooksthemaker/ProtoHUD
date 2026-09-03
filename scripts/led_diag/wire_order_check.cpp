// Headless check of the accessory-LED wiring order: permutation normalisation
// and the chain maths that turns it into per-zone start indices.
//   g++ -std=c++17 -Wall -Wextra -I<repo>/src wire_order_check.cpp \
//       <repo>/src/accessory/accessory_leds.cpp -o wire_order_check
// (only the free functions are exercised; no hardware, no threads)
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "accessory/accessory_leds.h"

using accessory::ZoneCount;
using accessory::ZoneConfig;

static bool fail = false;
static void expect(bool ok, const std::string& what) {
    std::printf("  %-56s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) fail = true;
}

static bool is_perm(const std::array<int, ZoneCount>& a) {
    bool seen[ZoneCount] = {};
    for (int v : a) {
        if (v < 0 || v >= ZoneCount || seen[v]) return false;
        seen[v] = true;
    }
    return true;
}

static std::string show(const std::array<int, ZoneCount>& a) {
    std::string s;
    for (int v : a) s += std::to_string(v) + " ";
    return s;
}

int main() {
    // ── normalise ────────────────────────────────────────────────────────────
    std::printf("normalize_wire_order\n");
    {
        auto r = accessory::normalize_wire_order({0, 1, 2, 3, 4});
        expect(is_perm(r) && r[0] == 0 && r[4] == 4, "identity is unchanged");
    }
    {
        auto r = accessory::normalize_wire_order({4, 3, 2, 1, 0});
        expect(is_perm(r) && r[0] == 4 && r[4] == 0, "reversed order preserved");
    }
    {
        // Duplicates: the later copy is dropped, missing zones appended in order.
        auto r = accessory::normalize_wire_order({2, 2, 2, 2, 2});
        expect(is_perm(r), "all-duplicates still yields a permutation");
        expect(r[0] == 2, "the first copy keeps its position");
        std::printf("      -> %s\n", show(r).c_str());
    }
    {
        // What a short/hand-edited config array looks like after the -1 padding.
        auto r = accessory::normalize_wire_order({3, 0, -1, -1, -1});
        expect(is_perm(r), "padded-with--1 array yields a permutation");
        expect(r[0] == 3 && r[1] == 0, "the entries given are honoured");
        std::printf("      -> %s\n", show(r).c_str());
    }
    {
        auto r = accessory::normalize_wire_order({99, -5, 1, 1, 0});
        expect(is_perm(r), "out-of-range entries are dropped, not kept");
        std::printf("      -> %s\n", show(r).c_str());
    }

    // ── chain ────────────────────────────────────────────────────────────────
    std::printf("\nchain_zones\n");
    auto make = [] {
        std::array<ZoneConfig, ZoneCount> z{};
        const int counts[ZoneCount] = { 37, 37, 77, 77, 17 };   // the live rig
        for (int i = 0; i < ZoneCount; ++i) z[i].count = counts[i];
        return z;
    };
    {
        auto z = make();
        const int total = accessory::chain_zones(z, {0, 1, 2, 3, 4});
        expect(total == 245, "enum order totals 245 LEDs");
        expect(z[0].start == 0 && z[1].start == 37 && z[2].start == 74 &&
               z[3].start == 151 && z[4].start == 228,
               "enum order reproduces the rig's known starts");
    }
    {
        // Reorder: blush first, then the fins, then the hubs.
        auto z = make();
        const int total = accessory::chain_zones(z, {4, 2, 3, 0, 1});
        expect(total == 245, "reordering does not change the total");
        expect(z[4].start == 0,   "blush (first on chain) starts at 0");
        expect(z[2].start == 17,  "left fin follows blush");
        expect(z[3].start == 94,  "right fin follows left fin");
        expect(z[0].start == 171, "left hub follows the fins");
        expect(z[1].start == 208, "right hub is last");
        std::printf("      starts: ");
        for (int i = 0; i < ZoneCount; ++i) std::printf("z%d=%d ", i, z[i].start);
        std::printf("\n");
    }
    {
        // No overlaps, and full coverage of [0, total) — the property that
        // actually matters: every LED belongs to exactly one area.
        auto z = make();
        const int total = accessory::chain_zones(z, {3, 1, 4, 0, 2});
        std::vector<int> owner(total, -1);
        bool clash = false;
        for (int i = 0; i < ZoneCount; ++i)
            for (int k = 0; k < z[i].count; ++k) {
                const int idx = z[i].start + k;
                if (idx < 0 || idx >= total || owner[idx] != -1) { clash = true; break; }
                owner[idx] = i;
            }
        bool covered = true;
        for (int v : owner) if (v == -1) covered = false;
        expect(!clash, "no two areas claim the same LED");
        expect(covered, "every LED on the strip belongs to an area");
    }
    {
        // An empty area must take no chain space but still hold a sane start.
        auto z = make();
        z[4].count = 0;
        const int total = accessory::chain_zones(z, {4, 0, 1, 2, 3});
        expect(total == 228, "an empty area consumes no LEDs");
        expect(z[4].start == 0, "the empty area still has a valid start");
        expect(z[0].start == 0, "the next area starts at 0, not after a gap");
    }
    {
        // A malformed order must still chain everything exactly once.
        auto z = make();
        const int total = accessory::chain_zones(z, {1, 1, 1, 1, 1});
        expect(total == 245, "a degenerate order still chains all five areas");
    }

    std::printf("\n%s\n", fail ? "*** SOME CHECKS FAILED ***" : "ALL CHECKS PASSED");
    return fail ? 1 : 0;
}

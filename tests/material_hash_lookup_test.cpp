#include "../source/material_hash_lookup.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    MaterialHashLookup index;
    const auto emptyRevision = index.Revision();
    index.Observe("owned", 1, 0);
    Check(index.Revision() == emptyRevision, "unresolved observation is not an owner");
    index.Observe("owned", 1, 17);
    const auto ownedRevision = index.Revision();
    for (int n = 0; n < 1000; ++n) index.Observe("owned", 1, 17);
    Check(index.Revision() == ownedRevision, "repeated binds leave revision stable");
    index.Observe("owned", 2, 17);
    Check(index.Revision() == ownedRevision, "second variant is still one owner");
    index.Observe("foreign_stage1", 1, 17);
    Check(index.Revision() != ownedRevision, "late owner invalidates cached exclusivity");
    Check(index.Find(17) == std::vector<std::string>({"foreign_stage1", "owned"}),
        "all-stage owners are sorted and unique");
    index.Observe("owned", 1, 18);
    Check(index.Find(17).size() == 2, "other animation variant retains old hash owner");
    index.RemoveMaterial("foreign_stage1");
    Check(index.Find(17) == std::vector<std::string>({"owned"}), "invalidation removes foreign owner");
    index.RemoveMaterial("owned");
    Check(index.Find(17).empty() && index.Find(18).empty(), "invalidation removes all owner variants");
    const auto invalidatedRevision = index.Revision();
    index.RemoveMaterial("missing");
    Check(index.Revision() == invalidatedRevision, "missing invalidation is idempotent");
    index.Clear();
    Check(index.Revision() != invalidatedRevision, "empty cache reset invalidates old map revision");
    index.Observe("reused_pointer", 1, 19);
    Check(index.Find(17).empty() && index.Find(19).size() == 1,
        "pointer reuse after map reset cannot resurrect prior names");

    // Differential oracle scans a separate material->variant model. This covers
    // shared pointers, animation overlap, in-place hash changes and cache resets
    // against the old lookup semantics, without using the index's implementation.
    std::map<std::pair<std::string, uintptr_t>, uint64_t> observed;
    index.Clear();
    std::mt19937 random(0xA57A);
    for (int step = 0; step < 6000; ++step) {
        const std::string name = "material_" + std::to_string(random() % 12);
        const uintptr_t texture = 1 + random() % 8;
        const uint64_t hash = random() % 31;
        const auto action = random() % 100;
        if (action == 0) {
            index.Clear(); observed.clear();
        } else if (action < 14) {
            index.RemoveMaterial(name);
            for (auto it = observed.begin(); it != observed.end();) {
                if (it->first.first == name) it = observed.erase(it); else ++it;
            }
        } else {
            index.Observe(name, texture, hash);
            observed[{name, texture}] = hash;
        }
        for (uint64_t query = 1; query < 31; ++query) {
            std::set<std::string> expected;
            for (const auto& item : observed)
                if (item.second == query) expected.insert(item.first.first);
            const auto actual = index.Find(query);
            Check(actual == std::vector<std::string>(expected.begin(), expected.end()),
                "indexed lookup matches full-scan oracle after mutation");
        }
        Check(index.Find(0).empty(), "zero never enters reverse index");
    }
    std::cout << "PASS: ownership invariants and 180000 differential lookups\n";
}

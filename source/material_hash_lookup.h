#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// CPU-only ownership index. The caller provides synchronization and owns all
// texture lifetimes; identities are never dereferenced here. Refcounts preserve
// an owner when several animation/stage variants resolve to the same hash.
class MaterialHashLookup {
public:
    void Observe(const std::string& name, uintptr_t texture, uint64_t hash) {
        auto& previous = m_variants[name][texture];
        if (previous == hash) return;
        if (previous) RemoveOwner(previous, name);
        previous = hash;
        if (hash && ++m_owners[hash][name] == 1) ++m_revision;
    }

    uint64_t HashFor(const std::string& name, uintptr_t texture) const {
        auto material = m_variants.find(name);
        if (material == m_variants.end()) return 0;
        auto variant = material->second.find(texture);
        return variant == material->second.end() ? 0 : variant->second;
    }

    void RemoveMaterial(const std::string& name) {
        auto material = m_variants.find(name);
        if (material == m_variants.end()) return;
        for (const auto& variant : material->second)
            if (variant.second) RemoveOwner(variant.second, name);
        m_variants.erase(material);
    }

    void Clear() {
        m_variants.clear();
        m_owners.clear();
        ++m_revision; // Map/cache resets also invalidate callers holding an empty view.
    }

    std::vector<std::string> Find(uint64_t hash) const {
        std::vector<std::string> names;
        auto owners = m_owners.find(hash);
        if (owners != m_owners.end()) {
            for (const auto& owner : owners->second) names.push_back(owner.first);
            std::sort(names.begin(), names.end());
        }
        return names;
    }

    uint64_t Revision() const { return m_revision; }
    size_t MaterialCount() const { return m_variants.size(); }
    size_t HashCount() const { return m_owners.size(); }
    size_t VariantCount() const {
        size_t result = 0;
        for (const auto& material : m_variants) result += material.second.size();
        return result;
    }

private:
    void RemoveOwner(uint64_t hash, const std::string& name) {
        auto owners = m_owners.find(hash);
        auto owner = owners->second.find(name);
        if (--owner->second == 0) {
            owners->second.erase(owner);
            ++m_revision;
        }
        if (owners->second.empty()) m_owners.erase(owners);
    }

    uint64_t m_revision = 1;
    std::unordered_map<std::string, std::unordered_map<uintptr_t, uint64_t>> m_variants;
    std::unordered_map<uint64_t, std::unordered_map<std::string, size_t>> m_owners;
};

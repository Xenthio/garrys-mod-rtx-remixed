# Material ownership lookup regression

Build `material_hash_lookup_test.cpp` as a standalone C++17 executable. It uses
only the production `source/material_hash_lookup.h` and the C++ standard library.
The deterministic full-scan oracle checks 180,000 results across cache changes.

The native Lua API remains compatible with callers using the first return of
`RemixMaterial.FindMaterialByHash(hash)`. It additionally returns the matching
ownership revision as a plain hexadecimal string. `GetMaterialHashRevision()`
returns the current revision, or nil when a coherent refresh is unavailable.
Neither revision nor hash should be converted to a Lua number.

A caller caching ownership must poll the revision and invalidate its decisions
when it changes. New render observations and explicit invalidation update the
index immediately; a refresh at most once per second catches texture hashes
changing in place. The refresh snapshots COM references under the tracker mutex,
queries Remix outside that mutex, and rejects results if the cache changed in
the meantime. A hash/API failure returns nil revision, never a verified stale
revision. Newly discovered zero hashes remain unresolved until a later query.

`GetMaterialHashStats()` is a read-only process-lifetime counter snapshot. It
reports `lookup_calls`, `revision_polls`, `refreshes`, `hash_calls`, `cache_hits`,
`discarded_refreshes`, `failed_refreshes`, indexed `materials`, `variants`,
`hashes`, and `available`/`revision`. `hash_calls` counts only periodic ownership
refresh work; it deliberately does not claim to measure every tracker/API query.

In-game validation should check late hash sharing and runtime invalidation, a map
change, silent default logging, and debug logging when `r_remix_material_debug`
is enabled. Record counter deltas across many repeated lookups: full refreshes
should scale with elapsed seconds, not the number of hashes being looked up.

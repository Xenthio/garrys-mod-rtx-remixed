# Advanced Material Editor RTX bridge

This directory contains the Win64 compatibility provider used by Advanced
Material Editor. It is compiled into `RTXFixesBinary`; it is not a second
Garry's Mod binary module.

The provider exposes the global `AdvMatRTXBridge` protocol 1 table to client
Lua:

- `Capabilities()`
- `ApplyLegacyMaterial(request)`
- `ClearLegacyMaterial(request)`
- `ClearAllOwned({ protocol = 1 })`

### Protocol 1 shape

`ApplyLegacyMaterial` accepts one transaction containing `protocol = 1`,
`operation = "commit"`, a stable `target.material` string, one to 128 exact
64-bit hexadecimal strings in `hashes`, and material data in
`compiled.pbr` (or the equivalent `profile.pbr` compatibility field):

```lua
local ok, resultOrError = AdvMatRTXBridge.ApplyLegacyMaterial({
    protocol = 1,
    operation = "commit",
    target = { material = "models/example/material" },
    hashes = { "0123456789ABCDEF" },
    compiled = { pbr = {
        constants = { roughness = 0.4, metallic = 0.0 },
        maps = {
            albedo = "<UTF-8 game-root-contained path>",
            normal = { source = "<path>", ops = {} },
        },
        options = { filter_mode = "linear", wrap_u = "repeat", wrap_v = "repeat" },
    } },
})
```

`ClearLegacyMaterial` accepts `protocol`, `target`, and the exact `hashes` to
remove; `ClearAllOwned` accepts only the protocol and never accepts a path.
Success returns `true` plus a table containing `code`, `profileId`,
`layerPath`, `layerPaths`, and `importedTextures`. Failure returns `false`
plus a `code: message` string. `Capabilities()` reports protocol support,
writer/reset features, texture conversion support, configured limits, and the
fixed owned mod directory. The authoritative field parsing and complete PBR
surface are defined by
[`lua_bindings.cpp`](src/lua_bindings.cpp) and
[`bridge.h`](include/advmat_rtx_bridge/bridge.h).

The public Remix material API creates materials for Remix API-owned geometry.
This bridge provides the separate operation the addon needs: persistent PBR
replacement of already-captured Source draw-call texture hashes.

## Ownership and safety

The writer owns only:

```text
<Garry's Mod>/rtx-remix/mods/!advanced_material_editor/
```

It never reads, changes, or removes `~gmod_topbr`. Exact 64-bit hashes remain
strings from Lua through USDA authoring. `mod.usda` is the only activation
ledger and is published after its immutable, content-revisioned dependencies.
Exact-hash clears cannot remove another replacement for the same Source
material.

Protocol 1 is commit-only. `livePreview` is false and preview requests are
rejected before filesystem work. `ClearAllOwned` atomically publishes an
empty root before retiring prior profile and texture generations, preventing
persistent overrides from leaking between server sessions.

Inputs are confined to validated game-root descendants. Default limits are
32 MiB per source/output texture, 2048 pixels per axis, four texture
operations, and 128 hashes per transaction. The published set is additionally
bounded to 4096 active hash profiles, 8192 referenced texture files, and 2 GiB
of referenced texture data. Limits are checked before root publication, so a
rejected transaction preserves the previous active material state. Immutable
replacement staging plus the single retired reset generation can temporarily
use up to four times the active texture budget on disk; the next transaction
prunes superseded active files and a later reset replaces the retired set.
If a pre-existing root already exceeds the configured profile limit, ordinary
apply/clear calls reject it, while `ClearAllOwned` remains available as the
authoritative recovery path. Its empty-root deactivation is constant-time;
retiring and cleaning the previous generation is bounded but may be linear in
the number of owned files.

## Pure writer tests

The tests do not load Garry's Mod or RTX Remix:

```powershell
cmake -S source/advmat_rtx_bridge -B out/advmat-rtx-bridge -A x64
cmake --build out/advmat-rtx-bridge --config Release
ctest --test-dir out/advmat-rtx-bridge -C Release --output-on-failure
```

They cover localized paths, path and value validation, structural DDS/WIC
ingestion and corrupt-cache repair, texture-operation limits, normal-map mip
normalization, atomic multi-hash publication and rollback, unchanged-commit
reuse, exact-hash/idempotent clears, aggregate-layer migration,
resource-quota rollback, and authoritative reset/quarantine rotation.

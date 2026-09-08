# Prepared PBR assets in GMA addons

Large maps can spend a long time unlit while client Lua decodes textures and
publishes watched material layers. This loader reads prepared DDS textures and
USDA from GMA addons before the renderer creates its mod search paths. It
publishes each map only after its dependencies verify. Subsequent launches reuse
the verified cache and preserve unchanged material-root timestamps.

The feature consists of an optional Windows x64 D3D9 startup proxy and startup
bindings in the regular RTXFixesBinary module. Prepared maps need no separate
native writer or material editor addon. The map's own Lua still controls its
lights, Source material state and fallback behavior.

## Build

Use Visual Studio 2022 C++ Build Tools, CMake 3.20 or later, and Python 3. To build
the normal client module, initialize the repository's submodules and use its
Premake build:

```powershell
git submodule update --init --recursive
.\premake5.exe --os=windows --gmcommon=./garrysmod_common vs2022
msbuild RTXFixesBinary.sln /p:Configuration=Release /p:Platform=x64 /m
```

Premake 5 beta 2 is the version used by the existing build workflow. Configure
the separate startup components from the repository root:

```powershell
cmake -S source/startup_assets -B build/startup -A x64
cmake --build build/startup --config Release --parallel
ctest --test-dir build/startup -C Release --output-on-failure
```

The regular **Build** workflow produces the client module in `windows-x64`.
**PBR GMA loader contracts** produces `pbr-gma-startup-x64`. The startup artifact
contains the wrapper, offline preparation tool and renderer contract; it does
not contain or replace the real Remix renderer. The fake renderer is test-only
and is never packaged.

## Installation

Close the target game first. Keep a backup of the installed client module and
original renderer outside the game directory. This wrapper forwards a fixed
export table: check the actual renderer before installing it. From the source
checkout, run:

```powershell
python source/startup_assets/generate_proxy_exports.py "<game>/bin/win64/d3d9.dll" --check
```

When using the downloaded startup artifact, run `python generate_proxy_exports.py
"<game>/bin/win64/d3d9.dll" --check` in its extracted directory. This command
checks the x64 PE exports, ordinals, binary size and SHA-256 without loading the
DLL or changing the contract. A failed check means that this prebuilt wrapper
must not be installed with that renderer. Developers supporting another renderer
must audit it, regenerate the contract, rebuild and test that combination.

The recorded renderer SHA-256 is
`6874ba37d27f6c4f88c7ddbbd513f5fe028f14fbe6bbc402e9a309ba835d0356`.
Its 146 original exports are preserved; the proxy adds two startup exports.

Install these components after the check succeeds:

| Source | Destination relative to the game root |
| --- | --- |
| Exact original renderer bytes from the backup | `bin/win64/d3d9_astra_renderer.dll` |
| Built `build/startup/Release/d3d9.dll` | `bin/win64/d3d9.dll` |
| Built `x86_64/Release/gmcl_rtxfixesbinary_win64.dll` | `garrysmod/lua/bin/gmcl_rtxfixesbinary_win64.dll` |

If an Astra wrapper is already installed, run the check against its
`d3d9_astra_renderer.dll` sibling instead. Preserve that original sibling; do not
copy the old wrapper over it. Verify the sibling still has the checked SHA-256
after copying. The offline `astra_rtx_prepare_assets.exe` may remain outside the
game; it is not needed at runtime.

Put companion GMA files together in an immediate addon folder, for example:

```text
garrysmod/addons/pbr_maps/gm_example.gma
garrysmod/addons/pbr_maps/gm_example_rtx.gma
```

The scanner discovers loose addons, direct `addons/*.gma` files and immediate
`.gma` files inside each addon folder. Prefer the folder layout above because
Source can move root-level archives into its Workshop cache. Workshop caches,
linked paths and deeper directories are not discovered by this implementation.
An arbitrary Workshop subscription is therefore not sufficient unless its
prepared assets also reside in a discovered physical location.

For explicit installations or testing, set `ASTRA_RTX_STARTUP_SOURCES` in the
game process's environment to a JSON array of absolute addon directory or GMA
paths. It replaces addon discovery. Otherwise `-noaddons` disables addon
sources. The game's own loose `garrysmod/data_static` is always considered.

## Package and runtime contract

This loads packages with a version 1
`data_static/astra/<map>/rtx/startup.json`; it does not convert arbitrary Source
VMTs or PNG-only packages into startup materials. Each manifest provides its map
name, 64-character generation, texture descriptors and one `mod.usda` descriptor.
Descriptors contain `path`, `target`, `bytes` and `sha256`. The layer is stored
as `mod.usda.dat`; prepared DDS payloads use `.dds.dat` inside `data_static` and
become content-addressed `textures/<sha256>.dds` files outside the GMA. Existing
compressed mip data passes through unchanged. See the native test fixtures for
executable package examples.

The owned output is `rtx-remix/mods/!astra_startup_<map>/`. Paths, descriptors,
duplicate members, declared hash ownership and layer dependencies are checked.
The visible root is committed only after its dependencies verify. Invalid,
removed or conflicting packages have their own roots deactivated. A recognized
legacy Astra root is backed up and deactivated before startup roots replace it;
unknown legacy root contents block publication. Other mod namespaces are left
alone.

`RemixStartupAssets` is available after loading RTXFixesBinary. An absent-only
`AstraRTXBridge` compatibility table supports existing prepared map addons. A
separately installed full bridge is loaded through Garry's Mod's normal protected
`require` before publishing that compatibility table; an already registered
provider is preserved. The startup table exposes:

| API | Result |
| --- | --- |
| `API_VERSION` | `1` |
| `Capabilities()` | Startup discovery capability; no live writer or batch capabilities |
| `GetStartupStatus()` | Copied JSON string, or `nil` if unavailable/oversized (2 MiB maximum) |
| `DisableStartupMap(map, generation)` | Boolean; scoped to the prepared generation |

The status identifies each prepared map and its generation. Map Lua must verify
that identity before treating its replacements as active. The compatibility
table's `ownedNamespace` is `astra_map_importer` for the existing addon protocol;
the proxy's actual output remains the separate per-map startup roots above.
This bridge does not implement the older map-time PNG writer. PNG-only fallback
packages still require their original optional writer; a map can otherwise keep
its baked Source materials when preparation is unavailable.

Material ownership queries now use a revisioned reverse index instead of querying
every texture for every requested hash. A periodic refresh queries unique
retained textures outside the tracker mutex and rejects results if the cache
changed. Tracking no longer calls `ITexture::Download()` just to poll a hash.
See [ownership API contracts](../tests/material_hash_lookup.md) for cache
invalidation and diagnostics.

## Validation and diagnostics

CTest exercises archive discovery, cache reuse, corruption, missing assets,
unsafe paths, hash conflicts, legacy migration, real proxy forwarding and Lua
startup contracts. The ownership oracle checks 180,000 results against an
independent full scan. These are native/contract tests; they do not create a GPU
device or claim a new in-game timing measurement.

The proxy keeps a startup snapshot; the scanner also writes
`garrysmod/data/astra/startup/status.json`. Inspect per-map `ready`, `generation`,
`errors`, `seconds`, `cache_hits` and `bytes_written`. An unchanged second launch
should reuse DDS files and avoid rewriting `mod.usda`. The cache uses verified
receipts and file size/mtime, rather than rehashing every archive payload on each
launch. A local modification that preserves both size and timestamp is outside
that cache assumption.

For an offline check with the game closed:

```powershell
build/startup/Release/astra_rtx_prepare_assets.exe --game-root "<game>"
```

This command prepares files and writes status; it is not a read-only inspection.
It can also take repeated `--source` paths or `--no-addons`. Before removing the
wrapper, remove its physical GMA sources and run a successful `--no-addons`
preparation to deactivate stale startup roots (also remove any relevant loose
game `data_static` package). Then restore the backed-up original renderer.
Keep any deactivation failure visible and resolve it before restoring a renderer
that would otherwise continue to load stale layers.

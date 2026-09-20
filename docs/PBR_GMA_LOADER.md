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

The regular **Build** workflow produces the client module and the startup loader
in `windows-x64`, and includes both in the nightly release ZIP. Startup files are
staged under `bin/win64/astra-startup/` so extracting the fixes package cannot
overwrite the Remix renderer. RTXLauncher versions with startup-loader support
verify the renderer contract after installing either package, preserve the real
renderer as `d3d9_astra_renderer.dll`, and install the wrapper as `d3d9.dll`.
Older launchers leave the staged files inactive; use the manual steps below or
update the launcher. Enabling Workshop mounting alone does not install the loader.

**PBR GMA loader contracts** also produces `pbr-gma-startup-x64` for manual
installation. Both artifacts contain the wrapper, offline preparation tool and
renderer contract. Neither contains the real Remix renderer or the fake test
renderer. Keep the staged files when upgrading: the launcher rechecks the actual
renderer and leaves an unsupported renderer unwrapped, with a progress warning.

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
`b2e687406b9b24e669d21cb0eebee26f55585c60666ceeb08863b32f14346c62`
(265,998,848 bytes, `sambow23/dxvk-remix-gmod` nightly `12759b3`, the renderer
installed by RTXLauncher during fresh-install testing). Its export names and
ordinals match the previous audited renderer; RVAs and the file digest changed.
Its 146 original exports are preserved; the proxy adds two startup exports.

The default `dxvk.conf` disables RTX IO for this renderer. In the Cathedral
Workshop test, enabling it stalled the render thread during texture uploads;
disabling it allowed the scene to render. The prepared DDS files use the normal
texture-streaming path. This is a compatibility default, not a fix inside RTX IO.
Mods using RTX IO compressed packages need a renderer that supports those
packages without the stall before re-enabling `rtx.io.enabled`.

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

Subscribe to the base map and its PBR companion on Steam Workshop, let Steam
finish both downloads, and fully restart the game. The loader reads the current
Steam user's enabled subscriptions and resolves their installed GMAs across
Steam libraries, including when RTX runs from a copied game installation.
Subscribers do not need to copy Workshop files into `garrysmod/addons`.

Manual companion GMA files may instead live together in an immediate addon
folder, for example:

```text
garrysmod/addons/pbr_maps/gm_example.gma
garrysmod/addons/pbr_maps/gm_example_rtx.gma
```

The scanner discovers loose addons, direct `addons/*.gma` files and immediate
`.gma` files inside each addon folder. Prefer the folder layout above for manual
installs because Source can move root-level archives into its cache. Workshop
discovery inspects only installed directories selected from Steam's subscription
metadata. Unsubscribed cached items, other users' subscriptions, Steam-disabled
items, items in `garrysmod/cfg/addonnomount.txt`, and incomplete updates are
excluded. Linked paths and arbitrary deeper directories are not traversed.

For explicit installations or testing, set `ASTRA_RTX_STARTUP_SOURCES` in the
game process's environment to a JSON array of absolute addon directory or GMA
paths. It replaces both local addon and Workshop discovery. Otherwise
`-noaddons` disables both sources, while `-noworkshop` disables only Workshop
discovery. The game's own loose `garrysmod/data_static` is always considered.

Steam's installation and active account are discovered from its Windows registry
entries. `libraryfolders.vdf`, the current user's
`userdata/<account>/ugc/4000_subscriptions.vdf`, and each library's
`appworkshop_4000.acf` identify installed subscriptions. If required metadata is
missing or invalid, discovery reports that condition instead of loading all
cached Workshop files. These local formats are validated against the supported
Steam layout and covered by fixtures; a future Steam format change may require
a loader update.

For portable installations or isolated tests, `ASTRA_RTX_STEAM_ROOT` and
`ASTRA_RTX_STEAM_USER` override the Steam root and numeric account ID. An explicit
Steam root never falls back to another installation from the registry. The
offline tool accepts the equivalent `--steam-root PATH --steam-user ACCOUNT_ID`
options. Normal subscribers need no overrides. Subscription or enable/disable
changes take effect on the next full game launch, when preparation runs again.

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

Layers can reference Remix's built-in `AperturePBR_Opacity.mdl` and
`AperturePBR_Translucent.mdl` by those exact names. Relative MDL paths and
addon-supplied shader modules remain rejected. This permits opaque and
translucent PBR map materials without bundling renderer shader files.

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

CTest exercises local and Workshop archive discovery, current-user and disabled
addon filtering, multiple Steam libraries, incomplete updates, cache reuse,
corruption, missing assets,
unsafe paths, hash conflicts, legacy migration, real proxy forwarding and Lua
startup contracts. The ownership oracle checks 180,000 results against an
independent full scan. These are native/contract tests; they do not create a GPU
device or claim a new in-game timing measurement.

The proxy keeps a startup snapshot; the scanner also writes
`garrysmod/data/astra/startup/status.json`. Inspect per-map `ready`, `generation`,
`errors`, `seconds`, `cache_hits` and `bytes_written`. The `workshop` section
reports discovery and skipped-item reasons. An unchanged second launch
should reuse DDS files and avoid rewriting `mod.usda`. The cache uses verified
receipts and file size/mtime, rather than rehashing every archive payload on each
launch. A local modification that preserves both size and timestamp is outside
that cache assumption.

For an offline check with the game closed:

```powershell
build/startup/Release/astra_rtx_prepare_assets.exe --game-root "<game>"
```

This command prepares files and writes status; it is not a read-only inspection.
It can also take repeated `--source` paths, `--no-addons`, or `--no-workshop`.
Before removing the
wrapper, remove its physical GMA sources and run a successful `--no-addons`
preparation to deactivate stale startup roots (also remove any relevant loose
game `data_static` package). Then restore the backed-up original renderer.
Keep any deactivation failure visible and resolve it before restoring a renderer
that would otherwise continue to load stale layers.

## Coexisting map addons

An addon must validate its current-map manifest before acquiring a native
provider or clearing replacements. An absent manifest means that loader has no
work on this map; it must not require legacy batch APIs just to become inactive.
The startup-only bridge deliberately does not expose `BeginBatch` or a live
writer. A malformed manifest or failed cleanup must remain an error, rather than
being reported as a successful handoff.

Shared Lua loaders that coordinate through `reset_complete` should report
`no_map_manifest` and completion when they own nothing, without calling
`ClearAllOwned`. Once they acquire resources, they must release only their own
resources before yielding to another loader. This avoids an unrelated subscribed
PBR map blocking the active map on a clean client. Existing Workshop loaders may
need an addon update; packaging the native startup loader cannot repair arbitrary
third-party Lua ownership logic.

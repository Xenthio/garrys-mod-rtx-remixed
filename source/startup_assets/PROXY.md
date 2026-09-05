# Astra startup proxy

The x64 proxy replaces `bin/win64/d3d9.dll` and forwards the renderer's exact
export table to an unchanged sibling, `d3d9_astra_renderer.dll`. Its two D3D9
creation entry points call `Prepare` once before calling the original renderer.
It does no work in `DllMain`, creates no rendering device itself, and introduces
no background image decoder. The normal renderer still owns texture streaming.

The exported `AstraStartupStatusJson()` C function returns a UTF-8 snapshot in a
thread-local string. Copy it before the next call on that thread. The exported
`AstraDisableStartupMap(map, generation)` returns a C++ `bool` and deactivates only
the matching prepared map after checking the on-disk generation receipt. These
functions use `__cdecl`; the two intercepted D3D9 functions preserve `WINAPI`.

`ASTRA_RTX_STARTUP_SOURCES`, when provided in the child process environment, is
a JSON array of absolute addon directory or GMA paths. It replaces ordinary addon
discovery. Without that variable, `-noaddons` selects no addon archives; otherwise
normal discovery runs. The game's own `data_static` directory remains available.
The status reports `source_selection` as `explicit_environment`, `noaddons`, or
`default_discovery`. An invalid explicit selection reports failure and attempts
to deactivate stale owned packages before continuing to the renderer.

## Building and checking

Use CMake with MSVC x64, then build all targets and run CTest. The DLL output is
`Release/d3d9.dll`; the fake renderer in `fake_renderer/Release` is test-only and
must never be installed as the real renderer. Tests exercise the real proxy in
fresh processes against that fake DLL, including all export names and ordinals,
argument forwarding, once-only preparation, source selection, and scoped failure
cleanup. They do not launch a game or create a GPU device.

`proxy_renderer_exports.json` records the exact renderer binary SHA-256 and 146
original export entries. `generate_proxy_exports.py PATH_TO_REAL_RENDERER`
regenerates this contract and `proxy_exports.def`; audit any new renderer before
distributing a regenerated proxy. Two additional Astra exports are appended.
Builds create an export object with `LIB /DEF` before linking because MSVC 19.44
rejects the mixed local/forwarded table during its combined import-library step.

Installation must back up and verify the user's actual renderer bytes, verify
that its export contract matches, and preserve those exact bytes as the sibling.
Do not overwrite that sibling with the test DLL or an older renderer. Real-game
validation is still required after any renderer change because tests cannot
prove unrelated modules' assumptions about the original DLL filename.

## Importer installation

The companion Astra map importer ships the reviewed wrapper and offline scanner
under `native/rtx_startup/dist`, with SHA-256 hashes, source provenance and the
complete renderer export contract in `manifest.json`. Its separate native Lua
helper lives under `native/rtx_bridge/dist`. The helper reads this proxy's startup
status and lets a lighting/hash failure disable only its prepared map.

From the importer directory, prepare an existing verified map as a new job:

```powershell
python import_map.py prepare-rtx-startup --source-job "build/gm_example" --output "build/gm_example_startup"
python import_map.py install --output "build/gm_example_startup" --gmod "H:\SteamLibrary\steamapps\common\GarrysMod_RTX_c"
```

Fresh `build` and `recompile` commands default to `--rtx-loading startup`.
Their `--install` option uses the same complete installation transaction. That
transaction verifies and stages every GMA companion, the matching Lua helper,
the proxy, an exact renderer sibling, and their receipts before publication. It
restores previous files together if publication fails. A running game from a
different verified installation can remain open; the target installation must
be closed. Automated RTX tests leave the initial menu open for at least ten
seconds before loading their first map.

The importer checks the actual PE export names, ordinals and forwarding targets
without loading the DLL. It preserves the original renderer under
`<game>/_astra_startup_backups/<sha256>_d3d9.dll` and refuses an unknown existing
`d3d9_astra_renderer.dll`. Its `plan_startup` function returns a read-only native
file plan for the map deployment transaction.

`install-rtx-startup` is a standalone service command that installs only this
wrapper. The matching Astra Lua helper must already be installed before loading
startup PBR maps. Normal map installation should use the complete `install`
command above, which handles both binaries and the content together.

`uninstall-rtx-startup --gmod GAME` verifies active `!astra_startup_*` layers
against their native receipts and startup-status generations, backs up and
empties those layers, then restores the exact original renderer. Failure restores
the layers and wrapper together. DDS caches, unrelated mods and RTX settings
remain unchanged; an unknown or manually changed active layer prevents uninstall
from replacing the renderer.

## Texture identities

Importer `astra/rtx_hashes.py` hashes the exact compiled VTF mip buffers. Source's
RGBA8888-to-BGRA8 upload conversion is applied before computing both XXH64 and
XXH3-64. Compressed DXT blocks remain compressed. Publication requires mip 0;
only mip 1 and mip 2 are optional aliases, and both dimensions of an optional
mip must remain at least 128 texels. `NOMIP` or `NOLOD` VTF flags disable optional
aliases. Smaller or otherwise excluded variants remain provenance records and
are never published globally. The package builder rejects ambiguous full-size
identities and omits ambiguous optional aliases.

The byte-level algorithm is in the installed renderer fork's
[D3D9CommonTexture::SetupForRtxFrom](https://github.com/sambow23/dxvk-remix-gmod/blob/384581b1/src/d3d9/d3d9_common_texture.cpp#L655-L686).
The initial proof matches all 78 observed Bank/gallery texture identities with
the compiled VTF bytes using XXH64 at `mat_picmip 0`. XXH3, compressed formats,
and optional quality aliases remain provisional until their specific reduced
texture-quality settings have been validated live; this does not claim arbitrary
`mat_picmip` settings or every hardware configuration.

# Startup proxy internals

See [PBR GMA loader](../../docs/PBR_GMA_LOADER.md) for standalone build,
installation, package layout and Lua API instructions.

The Windows x64 proxy occupies `bin/win64/d3d9.dll` and forwards the renderer's
exact export table to an unchanged sibling, `d3d9_astra_renderer.dll`. Its two
D3D9 creation entry points call `Prepare` once before calling the renderer. It
does no work in `DllMain`, creates no device and introduces no image decoder.
The renderer continues to own texture streaming.

`AstraStartupStatusJson()` returns a UTF-8 snapshot in a thread-local string.
Copy it before the next call on that thread. `AstraDisableStartupMap(map,
generation)` returns a C++ `bool` and deactivates only the matching prepared map
after checking its on-disk generation receipt. Both functions use `__cdecl`;
the intercepted D3D9 entry points preserve `WINAPI`.

`ASTRA_RTX_STARTUP_SOURCES` is an optional JSON array of absolute addon directory
or GMA paths, replacing ordinary local and Workshop addon discovery. Without it,
`-noaddons` disables both, while `-noworkshop` disables only Workshop subscriptions;
loose game `data_static` is still considered. Status records
`source_selection` as `explicit_environment`, `noaddons` or `default_discovery`.
An invalid explicit selection reports failure and attempts scoped deactivation
before continuing to the renderer.

Workshop discovery runs before the first D3D9 creation call. It resolves the
current user's cached subscriptions through Steam's library and installation
metadata, then reads immediate GMA files from the selected item directories.
Disabled, unsubscribed and incomplete items are excluded; arbitrary cached items
are never used as a fallback when subscription metadata is unavailable. Portable
Steam root/account overrides are documented in the setup guide. No Steam API is
initialized and no downloads are started by this proxy.

The recorded export contract contains 146 original names and ordinals, plus two
startup exports. `generate_proxy_exports.py RENDERER --check` verifies that exact
binary without changing anything. Omitting `--check` regenerates the contract
and `.def`; this is a developer operation requiring an audit and a new build.
The export object is generated with `LIB /DEF` separately because MSVC 19.44
rejects mixed local/forwarded exports in its combined import-library step.

The proxy tests load the real built wrapper in fresh processes against a fake
renderer. They check every export name and ordinal, argument forwarding,
once-only preparation, source selection and scoped failure cleanup. They do not
create a GPU device. The fake renderer is test-only and must never replace the
user's renderer. Real-game validation remains necessary when the renderer changes.

Prepared texture hashes must describe the exact Source-uploaded VTF mip bytes;
they are not filenames or material-name hashes. The original packager applies
RGBA8888-to-BGRA8 conversion and hashes compressed DXT blocks unchanged, according
to the renderer's texture identity algorithm. Optional mip aliases must be
validated by the package author and cannot overlap another installed package's
declared identities. This loader verifies the package manifest and dependencies;
it does not recompute GPU texture identities or claim compatibility with all
renderer versions and texture-quality settings.

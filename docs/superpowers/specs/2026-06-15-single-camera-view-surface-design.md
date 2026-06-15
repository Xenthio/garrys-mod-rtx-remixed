# Single-Camera View Surface Prototype Design

## Goal

Build a first dxvk/remix-side prototype for scope-like surfaces that raytrace through one explicit secondary camera. The first target is MWBase scope lenses, but the feature should stay general enough for other addons that need a surface to show a view that the Source renderer does not provide as an actual render target camera.

## Prototype Scope

The prototype supports one active view surface camera per frame. It does not support multiple simultaneous monitors, paired ray portals, recursive views, or per-surface camera arrays. Those are later features after the one-camera path proves useful in-game.

The prototype avoids git worktrees. Runtime validation is expected to happen in Garry's Mod. Local verification focuses on build checks and syntax checks that can run outside the game.

## Approach

Use the existing raytraced render target shader path as the base behavior. That path already handles the central operation: when a ray hits a flagged opaque surface, it uses the hit UVs to create a new ray from `renderTargetCamera`.

The missing capability is feeding `CameraType::RenderToTexture` without requiring the game to draw a real D3D9 render target scene. The prototype adds a public/internal API path that lets the GMod binary module submit an explicit render-view camera each frame.

## dxvk-remix Changes

Extend the Remix camera API so a caller can set the internal render-to-texture camera. The smallest version is to add one camera type value that maps to `CameraType::RenderToTexture`, then reuse `remixapi_SetupCamera`.

Keep the existing render target surface flag and shader path for the prototype. If a lens material can be categorized as raytraced render target usage, the current shaders can consume `renderTargetCamera` without adding a second shader feature.

If direct texture categorization is not enough for MWBase lens materials, add a narrow view-surface texture category that sets the same surface material flag as raytraced render targets. Do not add multi-camera indexing in the first pass.

## GMod Binary Module Changes

Update the public header copy used by the GMod binary module so it can call the new camera type. Add Lua binding coverage through the existing `RemixCamera` manager rather than adding ad hoc globals in `module.cpp`.

Expose a small Lua API:

```lua
RemixCamera.TYPE_RENDER_VIEW
RemixCamera.SetupParameterizedCamera({
    type = RemixCamera.TYPE_RENDER_VIEW,
    position = Vector(...),
    forward = Vector(...),
    up = Vector(...),
    right = Vector(...),
    fovYInDegrees = 15,
    aspect = 1,
    nearPlane = 1,
    farPlane = 16384
})
```

The current `SetupParameterizedCamera` binding always submits world camera data, so it must accept an optional `type` field and preserve existing behavior when the field is omitted.

## MWBase Patcher Changes

Add an opt-in path in `cl_mwbase.lua` that submits the active optic camera while ADS with a magnified optic. The camera should initially be derived from `EyePos()`, `EyeAngles()`, and the optic FOV metadata, with simple convars for offsets and FOV scale.

Keep the existing lens hiding and reticle compatibility code. This prototype only adds the raytraced view surface behind or through the lens; it does not replace all MWBase optic rendering at once.

## Verification

Local verification:

- Build `dxvk-remix-gmod` after the API and shader-side edits.
- Build `RTXFixesBinary` after the header and Lua binding edits.
- Run Lua syntax checks on edited Lua files.
- Run `git diff --check`.

In-game verification:

- Load a MWBase weapon with a magnified optic, including `mw_lwr` if available.
- Enable the prototype convar and ADS.
- Confirm the lens no longer appears blank when the active view-surface texture is categorized.
- Confirm scope view direction follows aim direction.
- Adjust prototype offset/FOV convars until the view is usable.
- Confirm non-scope weapons and red dot sights keep the existing behavior.

## Open Constraints

The first implementation may need a manual material/hash categorization step for the lens surface. Full automatic detection of all addon scope materials is outside the prototype.

The first implementation may only work for one active optic at a time. That is acceptable for first-person weapon scopes and keeps the shader/API surface small.

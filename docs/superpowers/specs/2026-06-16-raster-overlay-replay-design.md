# Raster Overlay Replay Design

## Goal

Render MWBase scope reticles as RTX-owned raster overlays instead of preserving the game's original draw calls after RTX injection.

## Problem

`rtx.rasterOverlayTextures` currently marks reticle textures and DXVK treats matching draws like late UI. That path triggers RTX injection and then preserves the original game draw. It is too coarse for scope reticles because a D3D9 draw can include state or geometry we do not want to preserve, and the runtime cannot control depth, blend, or clipping for the overlay.

## Approach

Keep the existing `rtx.rasterOverlayTextures` category as the material selector. In DXVK, matching draws should apply the original D3D9 raster state but skip the original draw. While that state is applied, DXVK snapshots the graphics pipeline/input/resource state and queues it as a raster-overlay draw. After RTX has composited and blitted the final image to the game target, DXVK restores each queued snapshot and replays it with controlled output, depth, blend, viewport, and scissor state.

The first prototype uses Vulkan/DXVK GPU rasterization, not a CPU software rasterizer. This gives us ownership of ordering and render state while reusing the existing D3D9 shader, vertex, texture, and draw-state machinery.

## Runtime Behavior

- A draw using a texture hash in `rtx.rasterOverlayTextures` is categorized as `RasterOverlay`.
- Raster-overlay draws apply D3D9 draw state but do not issue the original game draw.
- Raster-overlay draws are queued with `DrawParameters`, `DxvkContextState`, and shader resource slots.
- `RtxContext::endFrame` replays queued overlays after RTX has written to the game target.
- The initial replay pass disables depth/stencil tests but preserves the captured blend and material state.
- The queue is cleared at frame end even if RTX does not render a valid scene.

## Debugging

The prototype should include counters for captured, replayed, and skipped overlay draws. These counters are enough for in-game verification with MWBase reticle materials before we add scope-aperture clipping or primitive-level filtering.

## Verification

Automated verification is compile/static only because the visible behavior depends on the GMod client and RTX runtime. The in-game verification path is:

1. Confirm `rtx.rasterOverlayTextures` contains the MWBase reticle texture hash.
2. Aim down sights with an MWBase scope.
3. Confirm the reticle renders after RTX composition.
4. Confirm toggling the raster-overlay category removes/restores the reticle overlay.
5. Confirm the reticle no longer causes unrelated preserved game draws to appear after RTX injection.

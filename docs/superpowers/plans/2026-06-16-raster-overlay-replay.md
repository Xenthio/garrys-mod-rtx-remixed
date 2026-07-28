# Raster Overlay Replay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current preserve-original-draw raster overlay prototype with an RTX-owned GPU raster replay pass for reticle materials.

**Architecture:** D3D9 draw classification gets a `RasterOverlay` status. Matching draws apply their original D3D9 raster state but skip the original draw, then snapshot the applied DXVK graphics state/resources into `RtxContext`. `RtxContext::endFrame` replays queued overlay draws after RTX has written to the game target.

**Tech Stack:** C++17, DXVK D3D9, RTX Remix runtime, Vulkan graphics replay through existing `DxvkContext` APIs.

---

### Task 1: Add RasterOverlay Draw Classification

**Files:**
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/dxvk/rtx_render/rtx_types.h`
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/d3d9/d3d9_rtx.cpp`

- [x] **Step 1: Add `RasterOverlay` to `RtxGeometryStatus`**

```cpp
enum class RtxGeometryStatus {
  Ignored,
  RayTraced,
  Rasterized,
  RasterOverlay
};
```

- [x] **Step 2: Change raster-overlay classification to avoid early injection**

```cpp
if (checkBoundTextureCategory(RtxOptions::rasterOverlayTextures())) {
  return { RtxGeometryStatus::RasterOverlay, false };
}
```

- [x] **Step 3: Build expectation**

Run: `powershell -ExecutionPolicy Bypass -File .\build_dxvk_release.ps1`

Expected: C++ compilation reaches the next task's missing queue method references or completes if later tasks are already applied.

### Task 2: Queue Applied-State Overlay Draws

**Files:**
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/d3d9/d3d9_rtx.h`
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/d3d9/d3d9_rtx.cpp`
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/d3d9/d3d9_device.cpp`

- [x] **Step 1: Add a draw-prep flag**

```cpp
CommitToRasterOverlay = 1 << 3,
```

- [x] **Step 2: Route overlay status to apply state without original draw**

```cpp
if (status == RtxGeometryStatus::RasterOverlay) {
  return PrepareDrawFlag::ApplyDrawState | PrepareDrawFlag::CommitToRasterOverlay;
}
```

- [x] **Step 3: Queue the overlay snapshot from each draw-state lambda**

```cpp
if (drawPrepare & PrepareDrawFlag::CommitToRasterOverlay) {
  static_cast<RtxContext*>(ctx)->commitRasterOverlay(params);
}
```

Expected: Overlay draws apply their raster state, queue a replay snapshot, and do not issue `OriginalDrawCall`.

### Task 3: Replay Overlays After RTX Final Blit

**Files:**
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/dxvk/rtx_render/rtx_context.h`
- Modify: `C:/Users/cr/proj/dxvk-remix-gmod/src/dxvk/rtx_render/rtx_context.cpp`

- [x] **Step 1: Add a queue type and methods to `RtxContext`**

```cpp
struct RasterOverlayDraw {
  DrawParameters params;
  DxvkContextState contextState;
  std::array<DxvkShaderResourceSlot, MaxNumResourceSlots> resourceSlots;
};

void commitRasterOverlay(const DrawParameters& params);
void rasterizeRasterOverlays(const Rc<DxvkImage>& targetImage);
void clearRasterOverlays();
```

- [x] **Step 2: Store queued overlay draws**

```cpp
std::vector<RasterOverlayDraw> m_rasterOverlayDraws;
```

- [x] **Step 3: Replay after the final blit**

```cpp
blitImageHelper(this, srcImage, targetImage, VkFilter::VK_FILTER_NEAREST);
rasterizeRasterOverlays(targetImage);
```

- [x] **Step 4: Clear the queue at frame end**

```cpp
clearRasterOverlays();
```

Expected: Overlay draws are replayed once per frame after RTX output reaches the game target.

### Task 4: Verify Build and Prepare In-Game Checks

**Files:**
- Modify only files from Tasks 1-3.

- [x] **Step 1: Run diff whitespace check**

Run in `C:/Users/cr/proj/dxvk-remix-gmod`: `git diff --check`

Expected: no output, exit code 0.

- [x] **Step 2: Build DXVK runtime**

Run in `C:/Users/cr/proj/dxvk-remix-gmod`: `powershell -ExecutionPolicy Bypass -File .\build_dxvk_release.ps1`

Expected: exit code 0 and updated `public/bin/d3d9.dll`.

- [ ] **Step 3: In-game verification commands**

```text
rtx_patcher_mwbase_dump_reticle_overlay force
rtx.rasterOverlayTextures
```

Expected: the reticle material hash appears in `rtx.rasterOverlayTextures`, ADS shows the reticle through the new replay pass, and unrelated original draws are not preserved.

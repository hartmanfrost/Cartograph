# Cartograph Re-Architecture — Implementation Status

> Honest status of the STAGE2 re-architecture as it stands on disk. Nothing here
> has been compiled (no engine in this environment) or run; "implemented" means the
> idiomatic UE5 C++ exists and is internally consistent against the contract headers
> and the SPEC, not that it is verified on the CSS fork. Every fork-unverified
> dependency is marked with a `// SPIKE(Qn)` referencing SPEC §10 Q1..Q14 and is
> collected in the checklist below.

---

## 0. What landed (file inventory)

**Shared spine (Core)** — `Public/Core/*` + `Private/Core/*`
- `CartographTypes.h` — `FBuildingHandle`, `FBuildingRecord`, `FClassDrawInfo`,
  `FDrawGeometry`, typed extras, `CartographCoords::*`. (contracts agent; frozen)
- `CartographBuildingStore.{h,cpp}` — SoA `TSparseArray` store, generational handles.
- `CartographSpatialGrid.{h,cpp}` — uniform fixed-cell hash grid keyed by handle.
- `CartographZBandIndex.{h,cpp}` — decoupled coarse Z-band filter.
- `CartographClassDrawTable.{h,cpp}` — per-class table + shared pure `ComputeDrawGeometry`.
- `CartographTileManager.{h,cpp}` — monotonic dirty-tile set + per-tile `uint64` versions.

**Render** — `Public/Render/*` + `Private/Render/*`
- `CartographCompositor.{h,cpp}` — Phase-2/A never-cancelled tiled FCanvas (DEFAULT).
- `SCartographMapView.{h,cpp}` — Phase-3/B Slate `MakeCustomVerts` (CVar-gated).
- `CartographInstanceRenderer.{h,cpp}` — Phase-4/C AbstractInstance + ortho capture
  (CVar-gated AND `#if CARTOGRAPH_WITH_ABSTRACTINSTANCE` compile-gated).

**Net** — `Public/Net/*` + `Private/Net/*`
- `CartographMapReplicator.{h,cpp}` — server snapshot authority +
  `UCartographMapReplicationComponent` per-PC driver over `ReliableMessaging`.

**Config** — `Public/CartographConfig.h` + `Private/CartographConfig.cpp`
- World bounds, `RENDER_TEXTURE_SIZE=4096`, grid/tile/Z-band geometry, CVars.

**Integration (THIS agent's owned files)**
- `Public/CartographGameInstanceModule.h` / `Private/CartographGameInstanceModule.cpp`
  — owns the spine; O(1) build hooks; O(N) streaming gather; compositor drive; CVar
  backend select; legacy config/UI/override machinery preserved.
- `Public/CartographModSubsystem.h` / `Private/CartographModSubsystem.cpp` — thin shim:
  attaches the replication component per-PC, binds the server replicator / client
  stores, drives the per-net-tick delta push.
- `Public/CartographRemoteCallObject.h` / `Private/CartographRemoteCallObject.cpp` —
  DEPRECATED no-op stub (slice protocol deleted).
- `Cartograph.Build.cs` — `ReliableMessaging` dep added; RenderCore/RHI/Slate present;
  `CARTOGRAPH_WITH_ABSTRACTINSTANCE` define gating Phase 4.
- `Cartograph.uplugin` — `ReliableMessaging` plugin dependency added.

**Integration fixes to non-owned files** (outright build-blockers, see §5)
- `Private/Util/CartographCanvasRenderItem.cpp` — repointed the scissor read from the
  deleted module members to `Compositor.GetScissorForCanvas`.
- `Private/Render/CartographInstanceRenderer.cpp` — wrapped the TU in
  `#if CARTOGRAPH_WITH_ABSTRACTINSTANCE` so the default build (no AbstractInstance
  plugin) compiles.

---

## 1. Per-phase status

### Phase 0 — Crash-at-source / transport (SPEC §9)
**Implemented.** The hand-rolled slice RCO is deleted (`UCartographRemoteCallObject`
is a no-op stub). All transfer routes through `FCartographMapReplicator` +
`UCartographMapReplicationComponent` over `ReliableMessaging`: int32 sizes / uint64
ids (no int16 overflow), per-PC component with disconnect teardown (no timeout-race
null-deref), one shared immutable `TSharedRef<const>` snapshot per tile (no per-join
full serialize), coalesced per-tile version pings (no reliable-multicast-per-event).
The join flow is manifest → distance-ordered tile stream with a shallow in-flight ring.

**NOT verifiable without a build / live server (gates Phase 0):** the entire
`ReliableMessaging` mod-wiring path — `GetFromPlayer` returns a component, the
transport reports Connected without the mod driving the handshake, and a mod-owned
`Cartograph.Net.*` tag registers without colliding with `FGReliableMessagingTags`
(SPIKE Q1). Also whether a GameFeature may `NewObject` + `RegisterComponent` a
`Within=PlayerController` component at the point we attach it (SPIKE Q1, ModSubsystem).

### Phase 1 — The shared spine (SPEC §9, load-bearing)
**Implemented.** `CurrentBuildingData` + `BuildingDataIndexRedirector` +
`CurrentBuildingQuadTree` + `OnBuildingDataAdd/Remove` are deleted. The build hooks
(`AddFromBuildableInstanceData` / `AddFromReplicatedData` / `AddBuildable` /
`InvalidateRuntimeInstanceDataForIndex` / `RemoveBuildable`) now do ONLY O(1) work via
`AddRecordFromTransform` / `RemoveRecord`: store mutate + grid/zband insert/erase +
`TileManager.MarkDirtyForBox` + (server) `BumpVersion`. The O(N²) by-value
`InitialBuildableGather` is replaced by `StreamingGather` — an O(N) const-ref pass over
`GetAllBuildablesRef()` + `GetAllLightweightBuildableInstances()` (no by-value engine-map
copy, no global sort). `OnZFilterUpdated` is O(bands+hits) via `Compositor.SetZFilter`
(no 256 MB clear). `BuildingCountMap` (UI `DoesBuildingExist`) is maintained O(1) via a
dense `ClassIdToHash` reverse.

**Known data-model divergences (intentional, must be diff-tested before trusting):**
the slim record drops Scale3D (scaled foundations/beams render at scale 1) and keeps
only yaw (pitch/roll dropped) — DIFF-TEST(Q12)(a)/(b) in `CartographClassDrawTable.cpp`.

**NOT verifiable without a build:** that `ComputeDrawGeometry` reproduces the legacy
`FillInCache` pixel-identically (SPIKE Q12 — diff-render a large save before the legacy
caches are considered safe to delete); that the position-probe removal
(`FindHandleForRemoval`) matches the engine's mass-dismantle correctly (SPIKE Q11).

### Phase 2 — Convergent compositor + bounded tiled FCanvas (DEFAULT backend)
**Implemented.** `FCartographCompositor` is bound + its never-cancelled `TickConverge`
loop started ONCE in `InitializeSpine`; it drains dirty tiles under `FTickTimeBudget`
(fraction `r.Cartograph.FrameBudgetFraction`), one Begin/EndDraw per tile (= a separate
GPU command buffer), await once per tile. The global `FCanvas::GetBatchedElements` hook
is repointed at `Compositor.GetScissorForCanvas` and is INERT for non-compositor canvases
(removes the legacy process-wide tax). `RENDER_TEXTURE_SIZE` is 4096 (64 MB) via Config.

**NOT verifiable without a build (gates Phase 2):** the persistent-RT preserve across
the 2nd+ `BeginDrawCanvasToRenderTarget` (the "lines going crazy" history) — SPIKE Q3.
The committed fallback (`ClearTileRectViaRHI_ELoad`, `ERenderTargetLoadAction::ELoad` via
a scissored RHI clear) is written but unexercised; `DrawClearQuad`/`FRHIRenderPassInfo`
export on the CSS RenderCore/RHI is unconfirmed. Also `UKismetRenderingLibrary::Begin/
EndDrawCanvasToRenderTarget` exported overload + `UCanvasRenderTarget2D::GetWorld()`
non-null (the compositor resolves the world from the RT, not `this`) — SPIKE Q3.

> **Atlas uasset manual step:** `RENDER_TEXTURE_SIZE` dropped 8192→4096 in Config; the
> persistent `UCanvasRenderTarget2D` uasset `SizeX/SizeY` MUST be edited to 4096 to
> match, and `bAutoGenerateMips` must be false (enforced defensively in code).

### Phase 3 — Slate `MakeCustomVerts` (CVar-gated, NOT default)
**Implemented but NOT wired into the live UI.** `SCartographMapView` compiles against
public Slate (Slate is a dep). It is selectable via `r.Cartograph.RenderBackend=1` but
the UMG map panel still samples the FCanvas atlas; wiring the leaf widget into the panel
is a content/Blueprint step left for when the spike passes.

**NOT verifiable without a build/spike:** `FSlateDrawElement::MakeCustomVerts` /
`SLeafWidget` symbol export + UMG compositing on CSS Slate, and the correct shared
resource form (FSlateBrush vs material handle) — SPIKE Q4. If it fails, Phase A (the
FCanvas compositor) is the permanent floor; no committed path depends on Phase 3.

### Phase 4 — AbstractInstance + orthographic capture (CVar + compile gated)
**Implemented but COMPILE-GATED OFF and NOT wired.** `CartographInstanceRenderer.cpp` is
wrapped in `#if CARTOGRAPH_WITH_ABSTRACTINSTANCE` (default 0 in `Cartograph.Build.cs`),
so the default build does NOT include `InstanceData.h`/`AbstractInstanceManager.h` and
the Phase-4 bodies are elided. The UCLASS header stays parseable so the reflected type
still registers; nothing references it. Selectable via `r.Cartograph.RenderBackend=2`
only after the flag + plugin dep are turned on.

**NOT enabled / NOT verifiable:** the AbstractInstance/Wwise transitive link (SPIKE Q6),
capture isolation `GetViewOwner`/`ShowOnlyActors`/`PrimitiveRenderMode` on the fork (the
POC never compiled — SPIKE Q5), `bEnableOrthographicTiling`/`NumXTiles` existence (SPIKE
Q5), high-churn handle-write throughput on a 1M cold load (SPIKE Q7), capture orientation
parity (SPIKE Q12). C is upside, never a committed guarantee.

---

## 2. CVar-gated / fallback summary

| Backend | CVar | Default | Gate |
|---|---|---|---|
| TiledCanvas (A) | `r.Cartograph.RenderBackend=0` | **YES (floor)** | Phase-2 preserve test (Q3); ELoad-RHI fallback committed |
| SlateInstanced (B) | `=1` | no | Q4 spike; falls back to A |
| InstancedCapture (C) | `=2` | no | `CARTOGRAPH_WITH_ABSTRACTINSTANCE=1` + Q5/Q6/Q7; falls back to A/B |

Other CVars: `r.Cartograph.FrameBudgetFraction` (0.15), `r.Cartograph.TilesPerFrame`
(8), `r.Cartograph.PersistSpatialCache` (false; save-cache not implemented — Q14, OFF).

**Backend selection note (integration):** the module wires only the TiledCanvas
compositor into the live render path today. `CartographConfig::GetRenderBackend()` is
read at `InitializeSpine` for logging; promoting B/C to actually drive the UI panel is a
follow-up gated on Q4 / (Q5+Q6+Q7) — the data spine under all three is identical, so the
switch is a render-target/widget swap, not a re-architecture.

---

## 3. What is NOT verifiable without a build (summary)

1. **Compilation on the CSS fork** — no engine here; every engine/plugin API signature
   is written against the SPEC's stated surface or the legacy code's prior usage.
2. **Pixel-identity** of `ComputeDrawGeometry` vs legacy `FillInCache` (Q12).
3. **`ReliableMessaging` auto-attach + Connected + tag** behavior (Q1) — gates all net.
4. **Persistent-RT preserve across EndDraw** (Q3) — gates the TiledCanvas correctness.
5. **Removal-probe fidelity** under engine mass-dismantle / lightweight renumbering (Q11).
6. **`TSparseArray` iterator/Add/Reserve surface** on the fork (Q13).
7. **`UE::Tasks::Launch` + `GetResult()` blocking** semantics on the fork (Q1 in net).
8. **Slate `MakeCustomVerts`** export (Q4) and **AbstractInstance/Wwise + capture
   isolation** (Q5/Q6/Q7) for the gated backends.

---

## 4. SPIKE checklist (every `// SPIKE(Qn)` → file → what to verify → SPEC §10)

> Collected tree-wide (UE5Coro vendored lib excluded). Line numbers approximate.

### Q1 — `ReliableMessaging` mod-wiring + `UE::Tasks` + soft-class load (CRITICAL; gates Phase 0)
- `Net/CartographMapReplicator.cpp:16,835,854,1183` — `GetFromPlayer` returns a live
  component; transport Connected without mod handshake; `RegisterTaggedMessageHandler`
  accepts a mod-owned native tag without colliding with `FGReliableMessagingTags`;
  handshake-before-bulk (no public "is connected" query — manifest buffered pre-Connected).
- `Net/CartographMapReplicator.cpp:677,706` — `UE::Tasks::Launch` native on 5.6;
  `TTask::GetResult()` does not deadlock if the task graph is single-threaded.
- `CartographModSubsystem.cpp:~113` — a GameFeature may `NewObject`+`RegisterComponent`
  a `Within=PlayerController` component post-BeginPlay, and its BeginPlay then fires.
- `Core/CartographClassDrawTable.cpp:92,268` — `TSoftClassPtr::LoadSynchronous` /
  `UFGSplineBuildableInterface` mod-reachable (legacy used both → low risk).

### Q2 — host random-access spatial API (CRITICAL)
- `Core/CartographClassDrawTable.cpp:115` — `AFGBuildable::GetCombinedClearanceBox()`
  mod-reachable per CDO. Mitigated by the host keeping the slim record as spatial
  backing (the grid), so no random-access engine spatial query is needed at runtime.

### Q3 — persistent-RT preserve across EndDraw (gates Phase 2)
- `Render/CartographCompositor.cpp:34,107,384,418,440,730` (+ header :220) — preserve
  across the 2nd+ BeginDraw; `Begin/EndDrawCanvasToRenderTarget` exported overload;
  `UCanvasRenderTarget2D::GetWorld()` non-null; the ELoad-via-RHI fallback's
  `DrawClearQuad`/`FRHIRenderPassInfo`/`ELoad` exported on the fork. **Acceptance test:
  draw tile A → EndDraw → draw tile B → EndDraw → read back A survived.**

### Q4 — Slate `MakeCustomVerts` / `SLeafWidget` (gates Phase 3; A is the floor if it fails)
- `Render/SCartographMapView.cpp:54,177,523` (+ header :26,97) — symbol export + UMG
  compositing; `FSlateVertex::Make` signature; shared resource form.

### Q5 — Phase-C capture isolation + ortho tiling (gates Phase 4; B is default if it fails)
- `Render/CartographInstanceRenderer.cpp` (many, ~:44..594) — `PrimitiveRenderMode` +
  `ShowOnlyActors` + `GetViewOwner` honored; ortho `CaptureScene`; `bEnableOrthographicTiling`
  exists+honors `NumXTiles` (CaptureSource=SceneColor); private `AAbstractInstanceManager`
  `SetInstanceFromDataStatic`/`SetLocalTransform` reachable.

### Q6 — AbstractInstance/Wwise link + no-Ak manager (gates Phase 4)
- `Render/CartographInstanceRenderer.cpp:18,250` (+ header :91) — the transitive
  AkAudio/Wwise link when `CARTOGRAPH_WITH_ABSTRACTINSTANCE=1`; `bUseAkGeometry=false`
  marker is a no-op for audio; collision-disabled HISM path.

### Q7 — AbstractInstance high-churn + NotThreadSafe (gates promoting C over B)
- `Render/CartographInstanceRenderer.cpp:52,452,628` (+ header :184) — 1M cold-load
  game-thread handle-write throughput within `FrameBudgetFraction`; `MarkDirtyDeferred`
  is actually a batch (currently a no-op in the plugin); shared-material cheap-dirty.

### Q9 — FIFO head-of-line + by-value/pending-connection amplification (high)
- `Net/CartographMapReplicator.cpp:78,536` — in-flight ring depth (2) + per-tick drain
  without an ack callback; load-test FIFO under simultaneous join + belt-drag.

### Q11 — lightweight-index renumbering / removal probe (low)
- `CartographGameInstanceModule.cpp:~879` — `FindHandleForRemoval` resolves the handle
  by a position probe (the SML remove hooks give class+transform, not our handle);
  validate against a real engine mass removal. A miss leaks one record (no crash).

### Q12 — `ComputeDrawGeometry` pixel-identity (low)
- `Core/CartographClassDrawTable.cpp:39,535,562` (DIFF-TEST) + Phase-C
  `CartographInstanceRenderer.cpp:~370,664` — diff-render a large save before deleting
  the legacy caches; tolerate the intentional scale/pitch-roll divergences (a)/(b).

### Q13 — `TSparseArray` surface on the fork (low)
- `Core/CartographBuildingStore.cpp:63,186` (+ header :170) — `Add(const T&)` returns
  the new index; `Reserve(int32)`; `CreateConstIterator()` exposes `GetIndex()` and
  skips freed slots (fallback: index `[0,GetMaxIndex())` + `IsAllocated`).

### Q8 / Q10 / Q14 — resolved by design, no committed-path spike
- Q8 (version = monotonic counter, not content hash) — honored in `TileManager`/replicator.
- Q10 (tiles mod-owned, decoupled from WP) — honored in Config (`TILES_PER_AXIS` etc.).
- Q14 (save-persistence) — default OFF; `r.Cartograph.PersistSpatialCache` exists but
  the persist path is intentionally NOT implemented (recomputable cache, ship only if
  cold-load proves a problem).

---

## 5. Cross-file API mismatches reconciled during integration

1. **ODR / duplicate constants.** The legacy `CartographGameInstanceModule.h` redeclared
   `WEST_BOUND_CENTIMETERS`…`RENDER_TEXTURE_SIZE` (8192!) which now live in
   `CartographConfig.h` (4096). Any TU including both (ClassDrawTable / Compositor /
   Replicator do, via `CartographTypes.h`) was a hard redefinition conflict. **Fixed:**
   the legacy duplicates are deleted from the module header, which now `#include`s
   `CartographConfig.h` as the single authority. The legacy `world_position_to_screen_position`
   / `screen_position_to_world_position` helpers are kept (the still-compiling legacy
   `CartographDataStructure.cpp` uses them) and now resolve the Config constants.

2. **`CartographCanvasRenderItem.cpp` read deleted module members.** It read
   `Instance->CurrentCanvas` / `Instance->ScissorArea` — both deleted from the module.
   **Fixed (non-owned file):** repointed to `Instance->Compositor.GetScissorForCanvas(Canvas, Area)`
   resolved on the game thread before the render command is enqueued. The compositor's
   `friend class FCartographCanvasRenderItem` makes its member accessible; the
   `GetScissorForCanvas` surface was added by the compositor agent precisely for this.

3. **`CartographInstanceRenderer.cpp` unconditionally included the AbstractInstance
   plugin headers**, which do not exist in the default build (plugin dep commented).
   **Fixed (non-owned file):** wrapped the whole TU in `#if CARTOGRAPH_WITH_ABSTRACTINSTANCE`.

4. **Replicator client-store binding had no public method.** The frozen
   `UCartographMapReplicationComponent` declares no entry point to bind the dedicated
   client's local slim store/grid/tile-manager; the net agent exposed it as a
   `CARTOGRAPH_API` free function `CartographNet::BindClientStores(...)`. The ModSubsystem
   forward-declares + calls it (no header edit). Reported by the net agent as a
   contract deviation; integration honors it as-is.

5. **`ServerNetTick` drive point.** The replicator's `PushPendingDeltas` is per-net-tick;
   the module exposes `ServerNetTick()` and the ModSubsystem `Tick` (10 Hz) drives it +
   `EnsureReplicationComponents`. `AModSubsystem` ticks (it is an `AActor`); ticking is
   enabled in the ctor.

6. **`mBuildableClassToInstanceArray` direct access → public accessor.** `StreamingGather`
   uses `AFGLightweightBuildableSubsystem::GetAllLightweightBuildableInstances()` (const
   ref) rather than the protected member the legacy gather touched.

> No frozen PUBLIC signature in any contract header was changed. The legacy config
> USTRUCTs (`FCategoryData`/`FSplineData`/`FWireData`/`FBuildLayerData`/…) and the config
> TMaps are preserved on the module because `CartographClassDrawTable.cpp` and the
> still-compiling legacy `CartographDataStructure.cpp` source them by reference.

---

## 6. Dead code retained (not deleted — not in owned set, still compiles)

- `Public/CartographDataStructure.h` / `Private/CartographDataStructure.cpp` — the legacy
  `FBuildingData` second-copy struct + `FillInCache` math. No LIVE code path references it
  after the rewrite (only `CartographClassDrawTable.cpp`'s `#include` for the FG taxonomy
  includes it transitively; the actual draw math was re-ported into `ComputeDrawGeometry`).
  It is dead weight but compiles. Safe to delete once a build confirms nothing else needs
  it; left in place because it is outside this agent's owned set and the diff-test harness
  (Q12) references it as the legacy oracle.
- `UCartographRemoteCallObject` — reduced to a no-op UCLASS stub (kept compiling in case a
  save / SML reference resolves the class).

---

## 7. Suggested build / validation order

1. **Compile the default build** (`CARTOGRAPH_WITH_ABSTRACTINSTANCE=0`, RenderBackend=0).
   Expect: spine + TiledCanvas + ReliableMessaging net link; Slate backend compiled but
   unwired; Phase-4 elided. Fix any fork-API signature drift surfaced by the SPIKE markers.
2. **Phase 0 net spike (Q1)** on a live dedicated server — the single biggest "secretly
   needs the game's cooperation" risk. If `GetFromPlayer` is null / not Connected, the net
   path is inert and the host/SP path still works; resolve before relying on dedicated join.
3. **Phase 1 validation** — `stat memory` flat across a 5-min belt place/remove repro;
   `AddRecordFromTransform`/`RemoveRecord` sub-ms at 200k and not rising; the Q12
   diff-render harness (large save, legacy vs new atlas) before trusting the cache deletion.
4. **Phase 2 preserve acceptance test (Q3)** — draw A/EndDraw/draw B/EndDraw/read-back A.
   If it fails, switch `DrawTileIntoCanvas`'s clear to `ClearTileRectViaRHI_ELoad`. RenderDoc:
   confirm no submission approaches the TDR window; megabase first-paint drains over frames.
5. **Phase 3 spike (Q4)** — 100k custom-vert quads through the UMG panel. Adopt only if it
   composites; else A remains the floor.
6. **Phase 4 (Q5/Q6/Q7)** — only after flipping `bCartographWithAbstractInstance=true`,
   accepting the Wwise link, and benchmarking the 1M cold-load handle-write throughput.

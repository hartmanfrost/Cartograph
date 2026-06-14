This is a synthesis task. All inputs are provided. I need to write the definitive engineering report as GitHub-flavored Markdown. No tools needed — the deliverable is the report text itself.

# Cartograph Performance & Resource Audit — Engineering Report

## 1. Executive Summary

Cartograph's performance, memory, and crash problems trace to **3 root causes plus 1 amplifier**, all in the data model and rendering pipeline. They are deeply interconnected: one design decision (storing positional array indices in the spatial index) cascades into the lag spikes, the memory leak, and the load hang.

1. **The quadtree stores positional indices into a Z-sorted array, so every mutation renumbers everything.** `CurrentBuildingData` is kept globally sorted by Z (only so the height slider can range-select). Insertion shifts all later indices, so a parallel `BuildingDataIndexRedirector` exists to patch the quadtree's stale indices. This forces an **O(N) full-array scan on every single add and every single remove**, makes the initial load **O(N²)**, and the redirector **never compacts** (`-1` tombstones leak for the whole session, so the scan length grows with *cumulative lifetime placements*, not live count). This is the primary engine of issue #14's growing lag spikes and the unbounded memory growth. **Fix this one thing (stable handles) and the leak, the O(N) per-event cost, and the O(N²) load all collapse at once.**

2. **Cartograph keeps a full second copy of the entire factory in RAM**, ~344–432 B per buildable in `FBuildingData`, of which ~152–192 B is *precomputed derived render data* (screen position, corners, rotation, size) that is a pure function of `Transform` + per-class data and is trivially recomputable at draw time. At 1M buildings this is ~360–430 MB of pure duplication of data the engine already holds in `AFGBuildableSubsystem` / `AFGLightweightBuildableSubsystem`.

3. **A fixed 8192×8192 RGBA8 render target costs a hard 256 MB VRAM**, allocated permanently regardless of factory size, and the entire factory is rasterized into it in **one unbounded GPU submission** — `co_await Budget` paces only the *game thread that emits* elements; all batched primitives flush to the GPU in a single pass. A megabase full redraw (~0.5–1M primitives) on one present interval exceeds the Windows ~2s TDR threshold → driver reset → **client crash (#14)**. Every redraw also issues a full-screen 256 MB opaque clear tile.

4. **(Amplifier) The redraw coroutine is cancel-restarted on every build event, so the map never converges mid-drag** — each event throws away partial draw work, and the non-cancellable O(N) ingestion block is re-paid every restart. Under a belt-drag the coroutine is in a permanent cancel→restart loop producing zero completed frames until the player stops.

**The server join crash (#9) is separate and simpler:** three unchecked `*TMap::Find()` dereferences in the RCO crash the dedicated server whenever a timeout/late-ack/rejoin races the per-player transfer entry, compounded by an `int16` slice-count that overflows past ~63 MB so large factories never finish joining.

**Single highest-leverage change:** replace the positional-index quadtree + redirector with a **stable-handle store** (`TSparseArray` slot = stable ID; quadtree keyed by that ID). This is ~40 lines touching only `OnBuildingDataAdd`/`OnBuildingDataRemove` and the two `GetElements` call sites, requires no wire-format or rendering change, and simultaneously kills the #14 lag spikes, the redirector memory leak, the O(N²) load hang, and the latent `CurrentBuildingData[-1]` out-of-bounds hazard.

---

## 2. Root-Cause Table

| Problem area | Mechanism (Big-O / bytes) | Evidence (file:line) | Issues | Verdict |
|---|---|---|---|---|
| **Redirector O(N) per add** | Full scan of redirector incrementing every `Index>=Pos` on each add; scan length = cumulative lifetime adds (never compacts). O(N)/add → **O(N²) initial load**. The reindex branch is *provably never taken* during the monotonic initial load — pure wasted work. | `CartographGameInstanceModule.cpp:1199-1205`; init loop `:522-526`; tombstone note `h:361-364` | #14, #3, #9 | **Confirmed** |
| **Redirector O(N) per remove** | Two stacked O(N) scans: linear forward scan over equal-Z bucket via full `FBuildingData==` compare (`:587-605`) + full redirector walk to tombstone + decrement (`:1217-1230`) + `RemoveAt` tail-shift (`:597`). 3× O(N)/remove → **O(K·N)** mass-dismantle. | `cpp:587-605`, `:1217-1230`, `:597` | #14, #10 | **Confirmed** |
| **Redirector memory leak** | Tombstoned to `-1` on remove, never `.Remove()`d; grows 4 B per *lifetime* add. Minor in bytes (~4 MB/1M placements) but the real cost is the ever-rising O(cumulative-adds) scan length. | `cpp:1224`; reset only at `:290,:468` | #14, memory | **Confirmed** (memory magnitude downgraded; CPU-scaling is the real harm) |
| **Cancel/restart thrash** | Every hook calls `RedrawMap(false)` → `Cancel()` in-flight coroutine (`:442`); restart re-pays the non-cancellable O(N) ingestion under `FCancellationGuard` (`:562`). Zero completed frames during a drag. | `cpp:142,173,202,234,264`; `:442`; `:561-617` | #14, #10 | **Confirmed** |
| **Second factory copy** | `TArray<FBuildingData>` ~344–432 B/building (layout-dependent; LWC doubles). Duplicates engine state read at gather. ~72 MB @200k, ~360–430 MB @1M. | `h:359`; sources `:383-388`; struct `DataStructure.h:75-109` | #14, memory | **Confirmed** (size raised to 344–432 B range) |
| **Precomputed draw caches** | `FNormalDataCache`+`FRectangleDataCache`+`VisualBoxCache` ~152–192 B/building, all derived from Transform + per-class data. Corner math computed *twice* (`FillInCache` `:423-433` and `FillInVisualBoxCache` `:526-540`). | `DataStructure.h:53-89`; `cpp:264-446` | #14, #10, memory | **Confirmed** |
| **256 MB render target** | 8192² RGBA8 = 268 MB VRAM, permanent. `bAutoGenerateMips=False`, `SRGB=False` (verified in uasset). | `h:35`; uasset `SizeX/Y=8192` | #14, memory | **Confirmed** (mip-chain +89 MB claim *refuted*; flat 256 MB) |
| **Unbounded single GPU pass (TDR)** | One `FCanvas` open across whole redraw, flushed once. ~5–9 primitives/building → ~0.5–1M primitives in one RHI submission > ~2s TDR. Budget paces game thread only. | open `:630`, flush `:901`; `CanvasRenderItem.cpp:108`; primitives `:800-831` | #14, #10 | **Confirmed** |
| **Full-screen clear every redraw** | Opaque `FCanvasTileItem` `{0,0}..{8192,8192}` per pass (~67M texels), even though scissor bounds already computed at `:641-645`. Should be a free HW clear or scissor-sized. | `cpp:653-660` | #14, #10, memory | **Confirmed** |
| **Global FCanvas hook** | `SUBSCRIBE(FCanvas::GetBatchedElements)` reroutes *every* canvas in the game through the mod's render item. Per-batch trampoline + virtual dispatch + `IsCartograph` check process-wide; unchecked `static_cast` of `RenderBatchArray.Last()` is a cross-mod hazard. | `cpp:322-354`; cast `:342`; `CanvasRenderItem.cpp:91` | #14 | **Confirmed** (memory-safety sub-claim refuted; overhead + cast hazard real) |
| **By-value engine-map copy at load** | `InitialBuildableGather` takes `mBuildableClassToInstanceArray` **by value** ("Intentional copies"). Transiently doubles the bulk of the factory in RAM + O(N) deep copy. | `cpp:455-456`; `:387` | memory, #3, #14 | **Confirmed** |
| **RCO null-deref (the #9 crash)** | `*InitialBuildingDataToSendPerPlayer.Find(PC)` with **no null check** at 3 sites; the 10 s per-slice timeout frees+removes the entry, a late ack then derefs null → server access violation. | `RCO.cpp:65,73,87`; timeout `:83-90` | #9 | **Confirmed** |
| **RCO int16 slice overflow** | `Slices` is `int64` but RPC arg/counter are `int16`; @2047 B/slice, overflow at ~32767 slices ≈ 63–67 MB → completion never fires → stuck join → timeout → null-deref. | `RCO.h:16,76,91`; `cpp:79,103` | #9, memory | **Confirmed** |
| **RCO per-join full serialize** | `Archive << CurrentBuildingData` synchronously on game thread per joiner (~3–15 MB typical, geometric realloc 2–3× peak), once per join *and per respawn*. Stacks per concurrent joiner. | `RCO.cpp:48`; respawn `module.cpp:284-296` | #9, memory | **Confirmed** (bytes cut ~3–5× — quantized NetSerialize ~24–28 B/normal, ~100 B/spline) |
| **NetSerialize fixed-2047 + stack leak** | Always serializes full 2047 B regardless of `Size`; `Memcpy` copies only `Size`, transmitting uninitialized stack tail. Bandwidth waste + info leak. | `RCO.cpp:15-20,76-78` | #9, memory | **Confirmed** |
| **Reliable multicast flood** | `ClientUpdateBuildingData` is `NetMulticast, Reliable` fired per redraw with full add/remove arrays; mass-build saturates each client's reliable channel; slow client → queue overflow/disconnect. | `ModSubsystem.h:32`; `module.cpp:927` | #14, #9 | **Confirmed** |
| **BufferWriter move/double-free** | Claimed rehash-UAF / double-free. | `RCO.cpp:40,48,52,67,88` | #9 | **Refuted** — RCO is per-PlayerController (`Within=FGPlayerController`), map holds ≤1 entry; non-owning buffer; single-threaded + `ClearTimer` before free. The *real* bug here is the null-deref, not the move. |

---

## 3. Architecture Critique

### 3.1 The central flaw: the `CurrentBuildingData` (Z-sorted) + `TQuadTree<int32>` + `BuildingDataIndexRedirector` triad

This triad is the root of the lag, the leak, and the load hang. One array conflates **three independent roles**:

- **Storage** (the master `FBuildingData` list),
- **Spatial culling** (the quadtree, for localized redraw queries),
- **Z-height filtering** (via global sort + `Algo::LowerBound`/`UpperBound`).

The fatal coupling is that **the quadtree stores positional indices into the Z-sorted array**. Two consequences fall out:

1. Keeping the array Z-sorted means every insert/remove shifts later indices (`operator<=>` compares only `Transform.Z`, `DataStructure.cpp:23-26`). To stop the quadtree's stored indices going stale, the `BuildingDataIndexRedirector` must be re-based on every mutation — an **O(N) scan** (`:1199-1205`, `:1217-1230`).
2. `TQuadTree<int32>` can't cheaply update an element's payload, so removal can't just rewrite the index — hence the redirector tombstone design, which **never compacts** and leaks.

So a single belt placement costs: `Algo::LowerBound` O(log N) (the cheap part) + `CurrentBuildingData.Insert` O(N) memmove + redirector O(N) reindex. A single belt removal costs *three* O(N) operations (equal-Z linear `==` scan + redirector scan + `RemoveAt` shift). Because the redirector grows with lifetime placements, **per-event cost rises monotonically across a session even when live building count is flat** — which precisely matches issue #14's "gets worse over ~1h" signature. The Z-filter that justifies all this sorting is used *rarely* (only on slider drag) yet taxes *every* build event.

There is also a **latent out-of-bounds bug**: `GetElements` returns redirector indices that may be `-1` tombstones; `cpp:691` dereferences them unconditionally, and `CurrentBuildingData[i]` at `:717` is saved only *incidentally* by the `Algo::Sort` (pushing `-1` to front) + Min-cutoff slice. Any future code path reading `BuildingsToDraw` before that slice indexes `CurrentBuildingData[-1]`.

**The fix decouples all three roles:** a stable-handle store (positions never shift), a spatial index keyed by stable ID (O(1) removal, no redirector), and a separate Z-index. This is the prerequisite for everything else.

### 3.2 The second-copy-of-the-factory problem

`FBuildingData` is a heavyweight ~344–432 B record per buildable, of which **~152–192 B (≈49%) is precomputed derived data** — `ScreenPosition`, `Size`, `Rotation`, the four `Corners[4]`, plus `VisualBoxCache` — all pure functions of `Transform` + per-class lookups, recomputable in tens of FLOPs + one hash lookup at draw time. The `DataCache` variant is sized by its largest member, so **every spline/wire pays ~160–216 B for a cache it doesn't use**. On the *host*, this entire store duplicates `AFGBuildableSubsystem::GetAllBuildablesRef()` and `AFGLightweightBuildableSubsystem` — the very arrays the gather reads at `cpp:383-388`. The host could iterate the engine arrays per dirty region and keep only a spatial index + Z-index; only dedicated-server *clients* (which lack the subsystem data) need a slim store, fed by the RCO. The wire format already proves the minimal record: `NetSerialize` sends only class hash + quantized position + compressed yaw and **deliberately drops Scale and the caches** (`DataStructure.cpp:120-143`), so a ~24–40 B record (classHash + `FVector3f` pos + yaw float + type + extra-index) is sufficient — an **8–15× shrink**.

### 3.3 The 8192² render target

256 MB VRAM is the single largest *fixed* memory cost and a prime TDR suspect. At `PIXEL_PER_CENTIMETER` over a 7.5 km world this is ~1.09 cm/px — far finer than a minimap needs. Dropping `RENDER_TEXTURE_SIZE` to `1024*4` (4096²) is a one-constant + one-asset change that **quarters VRAM to 64 MB and quarters every clear/draw bandwidth**, and `ORIGIN_UV`/`PIXEL_PER_CENTIMETER` are `constexpr` off the constant so the code stays self-consistent (the uasset `SizeX/Y` must be matched). Worse than the residency is the *per-event* cost: the full-screen opaque clear tile is a 67M-texel rasterized quad pushed through the batched-elements pixel pipeline on **every** redraw, when it could be a free hardware `ClearRenderTargetView` (full) or a scissor-sized clear (partial, bounds already at `:641-645`).

---

## 4. Recommended Algorithmic Roadmap

Phased and ordered by dependency. Each phase is independently shippable and testable.

### Phase 0 — Stop the bleeding (low risk, ~1–2 days, no data-model change)

| # | Change | Algorithm / data structure | Improvement | Fixes | Effort |
|---|---|---|---|---|---|
| 0.1 | **Null-guard the 3 RCO `Find` derefs** | `if (auto* P = Find(PC); P)` else log+return | Eliminates the #9 server crash outright | #9 | **S** |
| 0.2 | **Widen `int16` slice counters → `int32`; complete on bytes-received** | `uint32 TotalByteSize`; `Slices = ceil(TotalByteSize/Max)` | Large factories can finish joining; removes ~63 MB ceiling + off-by-one | #9 | **S** |
| 0.3 | **RCO logout/EndPlay teardown** | `TWeakObjectPtr` controller in timeout lambda; `AbortTransferForPlayer` on disconnect | Stops per-disconnect buffer+timer leak | #9 | **S** |
| 0.4 | **NetSerialize only `Size` bytes** | length-prefixed serialize | Kills ~2 KB/slice stack-tail leak + info-leak | #9 | **S** |
| 0.5 | **Scissor the clear tile** | size `ClearItem` to `{MinIntX,MinIntY}..{MaxIntX,MaxIntY}` (`:641-645`) instead of full screen; use HW `ClearRenderTarget2D` for full redraws | 67M-texel clear → few hundred for a single belt | #14, #10 | **S** |
| 0.6 | **`RENDER_TEXTURE_SIZE = 1024*4`** + match uasset `SizeX/Y=4096`; expose as config | constant change | 256 MB → 64 MB VRAM; 4× clear/draw bandwidth | memory, #14 | **S** |
| 0.7 | **Gate `ClientUpdateBuildingData`** when add+remove both empty; skip in-Initial clients | early-out | Removes reliable-multicast no-op flood | #14, #9 | **S** |
| 0.8 | **Bulk-build redirector at initial load** | single O(N) pass (reindex branch is provably never taken) | **O(N²) → O(N)** load; kills the load hang | #3, #1, #14 | **S** |

### Phase 1 — Stable-handle core (THE highest-leverage change; ~M, no rendering change)

**Depends on:** nothing. **Highest ROI; ship first after Phase 0.**

- **What:** Replace `TArray<FBuildingData> CurrentBuildingData` + `BuildingDataIndexRedirector` with a `TSparseArray<FBuildingData>` whose stable slot index is the quadtree payload (`TQuadTree<int32>` → keyed by stable ID).
- **Algorithm:** `OnBuildingDataAdd` → `TSparseArray::Add` + `quadtree.Insert(slotId, box)`; **delete** the O(N) reindex loop (`:1199-1205`). `OnBuildingDataRemove` → `quadtree.Remove(slotId, box)` + `RemoveAt(slotId)`; **delete** the O(N) redirector scan (`:1217-1230`) **and** the equal-Z `==` linear scan (`:587-605`) since identity is now the handle. Move the Z-filter onto a separate sorted Z-index (`TArray<TPair<float,BuildingId>>` or 1 m Z-bands) so storage no longer needs ordering — removes the `operator<=>` coupling.
- **Complexity:** per-event **O(N) → O(log N)** (quadtree) + O(1) (slot); initial load **O(N²) → O(N log N)**; mass-dismantle **O(K·N) → O(K log N)**.
- **Memory:** eliminates the unbounded redirector leak; per-event scan length is now *live* count, and never rises with churn.
- **Fixes:** #14 lag spikes, the leak, the load hang, the `[-1]` OOB hazard.
- **Risk:** verify the UE 5.3 `TQuadTree::Remove(const ElementType&, const FBox2D&)` signature before deleting the redirector; preserve Z draw-order for overlap resolution via the new Z-index. **Effort: M.**

### Phase 2 — Slim record + per-class table + dirty-rect convergence (~M)

**Depends on:** Phase 1.

- **What:** (a) Build `TMap<uint32, FClassDrawInfo> ClassDrawTable` once at gather (sized by *classes*, hundreds). (b) Extract `ScreenPosition`/`Corners`/`Size`/`Rotation` into a pure `ComputeDrawGeometry(record, classInfo)` called in the draw loop; **delete** `FNormalDataCache`/`FRectangleDataCache`/`VisualBoxCache` fields (kills the duplicated corner math). (c) Shrink to `FSlimBuildingRecord {classHash, FVector3f pos, float yaw, type, extraIndex}` with spline/wire/beam payloads in side-tables. (d) Pass the engine map **by const-ref** at load, not by value. (e) Pre-resolve and pin distinct icon textures before `BeginDrawCanvasToRenderTarget` so the draw loop never `co_await AsyncLoadObject` mid-pass.
- **Convergence:** replace `RedrawMap`'s cancel-restart (`:442`) with **debounced dirty-rect accumulation** — drain Pending into the structures continuously (cheap now), expand a dirty-tile set, and run at most one budgeted redraw per quiescence window. Never discard completed draw work. Add the explicit user-triggered full-redraw button (#10).
- **Memory:** record ~344–432 B → ~24–40 B (**8–15×**); 1M buildings ~360 MB → ~26 MB; host can drop the store entirely (iterate engine arrays per dirty cell).
- **Fixes:** memory complaint, #10 (true localized + budgeted redraw + explicit full redraw), #14 convergence. **Effort: M.**

### Phase 3 — Bound GPU cost / kill the TDR (~M–L)

**Depends on:** Phase 1 (cheap spatial queries). Two paths; do 3a first.

- **3a — Chunked GPU flush (~M, surgical, keeps the canvas pipeline):** In the draw loop, after a config `MaxPrimitivesPerFrame` (~20k) emitted primitives, `EndDrawCanvasToRenderTarget` → `co_await NextTick()` → re-`BeginDrawCanvasToRenderTarget` the *same persistent target* **without re-clearing**. Converts one unbounded RHI submission into `ceil(primitives/budget)` bounded submissions → **TDR structurally impossible**. *Risk:* the existing comment about "lines going crazy" requires the `NextTick` between flushes and verifying the 2nd `BeginDraw` doesn't implicitly clear the RT.
- **3b — Uniform hash grid + tiled dirty rendering (~L, end-state):** Replace `TQuadTree<int32>` with `TMap<FIntPoint,TArray<BuildingId>>` over the fixed world bounds (`h:28-31`, ~50 m cells) → O(1) insert/erase, O(overlap) query. Partition the RT into 512² tiles with a `TBitArray DirtyTiles`; per frame pop K tiles, scissor-clear+redraw each independently. Multi-region edits no longer balloon the union AABB. **Effort: M (3a) / L (3b).**

### Phase 4 — Networking protocol scale-up (~M, after Phase 0 hardening)

- **Shared snapshot:** one `TSharedRef<const TArray<uint8>>` regenerated on a dirty flag, pinned by all joiners → join memory **O(joiners·N) → O(N)**.
- **Windowed pull:** 4–8 MTU slices per ack → ~5–8× fewer RTTs (200k-building join: ~2000 round-trips → a few hundred).
- Keep the transport Reliable; add a 1-byte protocol version. **Do not** adopt unreliable sequenced deltas yet (resync state machine is the highest-risk piece and not needed for #9). **Effort: M.**

### Phase 5 (optional, L) — GPU instanced SceneCapture renderer

Only if Phase 2+3 still can't hold frame time on megabases. Port the `Cartograph_2.0` direction: per-class HISM on a hidden marker actor + orthographic `USceneCaptureComponent2D`; redraw = one debounced `CaptureSceneDeferred`; add/remove = O(1) `AddInstance`/`RemoveInstance`; Z-filter/layers become material params + `clip()`. Deletes the global `FCanvas` hook and the scissor render item. **This is the true end-state for redraw cost but is a full rendering rewrite over an unfinished POC — gate on art review.**

**Dependency graph:** Phase 0 (independent) → Phase 1 (independent, do first) → Phase 2 & 3a (parallel, both need Phase 1) → Phase 3b (needs Phase 2's slim store) → Phase 4 (needs Phase 0) → Phase 5 (needs Phase 1+2 store as the instance source).

---

## 5. What to Salvage from Branches

**`origin/partial_redraw` (WIP, pre-1.2):** Do **not** merge — self-described WIP, two refactors behind, and it never solved the clip-the-draw problem (`cpp:417` `TODO: Stencil` — its clear and draw are not co-clipped; the current scissor render item exists precisely to fix this gap). **Salvage as design confirmation only:** the batched dirty-rect (`UpdateArea`) model, the event-driven incremental quadtree, and the dual `Initialize`/`Redraw` time budgets — all of which the current branch already adopted. Its `TQuadTree<FBuildingData>` (by-value) is a **memory regression**, not a fix.

**`origin/Cartograph_2.0` + `MapCaptureComponent2D` (POC):** Do **not** finish/merge — it does **not compile** (`Niagara->SetAsset()` no-arg stub at `CartographGameWorldModule.cpp:109`, undeclared `Ism`, Niagara module commented out in `Build.cs:38`), has dual conflicting render paths, no incremental updates, no instance cleanup on demolish, and its real ortho setup lives in unread `.uasset` blueprints. **Salvage as validated direction (→ Phase 5)** plus three concretely portable pieces: (1) `GetBuildingBoundingBox` via `GetCombinedClearanceBox()` + CDO-walking fallback (`:189-275`) — backend-agnostic, port regardless; (2) the `GetViewOwner()` override — the minimal correct capture-isolation mechanism; (3) per-class instance batching (component count bounded by class, not instance).

**`origin/show_buildable_ui` (abandoned UI prototype):** Do **not** cherry-pick — it *worsens* memory (adds a per-building `TSoftObjectPtr<AFGBuildable>` to `FBuildingData`, `DataStructure.h:93`, accumulating stale paths under churn) and adds no redraw optimization. **Two takeaways only:** (1) it proves the quadtree+redirector can serve fast point/region queries (validates the localized-redraw foundation); (2) if interactive pickup is wanted later, resolve the actor *lazily for the single clicked building*, never store a soft pointer per record. The EnhancedInput map-open gating pattern is reusable for the #10 full-redraw button.

---

## 6. Risks & Validation

**How to measure (define these before changing anything):**

- **Memory:** `stat memory` / `memreport -full` before and after a long build-and-dismantle session (the #14 repro: place/remove a belt for >5 min). Phase 1 success criterion: **flat** resident memory across churn (no upward drift). Track `CurrentBuildingData` byte size and confirm the redirector is gone.
- **Frame time / lag spikes:** `stat unit` + profile `OnBuildingDataAdd`/`OnBuildingDataRemove`. Success: a single belt placement no longer scans the full array; per-event cost is flat as the session ages (not rising). Target: sub-millisecond ingestion at 200k buildings.
- **VRAM:** GPU stats / `r.RHISetGPUCaptureOptions`; confirm 256 MB → 64 MB after Phase 0.6.
- **TDR / draw cost:** RenderDoc — confirm no single GPU submission exceeds the per-frame primitive budget after Phase 3a; reproduce the megabase full redraw and confirm it drains over many bounded frames without device reset.
- **Join time / #9:** on a dedicated server, force the timeout race (large factory + a client that stalls/disconnects mid-transfer) and confirm null-guards log-and-return instead of crashing; measure join wall-clock before/after windowed pull.

**What could regress:**

- **Phase 1:** dropping the global Z-sort changes draw order — verify overlapping icons/foundations still layer correctly via the new Z-index. `TSparseArray` slot reuse changes `BuildingId` stability semantics — add a generation counter if the RCO/UI ever keys on ID across remove+add.
- **Phase 2:** the draw-time `ComputeDrawGeometry` must reproduce `FillInCache` math *pixel-identically* — extract a shared pure helper used both ways during transition, verify on a large save, then delete the stored fields. Single-precision `FVector3f` position has ~0.05 cm worst-case error at the 7.5 km map edge — visually irrelevant, but remove any exact-equality position compares.
- **Phase 3a:** the 2nd `BeginDraw` must preserve target contents; a cancel mid-chunk leaves a partial map until the next completion (self-heals).
- **Phase 0.6:** resizing `RENDER_TEXTURE_SIZE` *must* match the uasset `SizeX/Y` or transforms desync (code is self-consistent via `constexpr`, the asset is the manual step).
- **Phase 4:** the shared snapshot must invalidate on every mutation (else stale late-joiners), and builds occurring *during* the pull must be replayed to the joiner (else missed buildings).

**Bottom line:** Phase 0 + Phase 1 alone resolve both crashes and the lag spikes at low risk and small diff. They are the mandatory first ship. Everything else is incremental scaling on top of a now-correct data model.

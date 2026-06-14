# Cartograph — Ground-Up Re-Architecture

> **Status:** Target architecture (end-state) and the dependency-ordered plan to reach it. This is not a patch list; it is the design Cartograph should *be*. The Stage-1 diagnosis (`docs/audit/STAGE1-diagnosis-report.md` §1–3 + root-cause table) and `problemDigest.json` are treated as verified ground truth and are cited throughout.
>
> **Engine premise correction (verified on disk).** The prompt assumed UE 5.3.2. `FactoryGame.uproject` declares `EngineAssociation="5.6.1-CSS"`. Every modern technique below is re-tagged against **5.6.1-CSS**, which removes most "needs-newer-UE" gates (native orthographic SceneCapture tiling, current `AbstractInstance` per-instance custom data, `UE::Tasks`, GPU-Scene auto-instancing). The hard caveat that survives: **there is no `Engine/` source in this repo** (only `Source/FactoryGame`, targets, and `Plugins/`), so "5.6 ships API X" is *not* proof that X is public/exported on CSS's forked binary. Every engine-internal dependency below carries a reachability note, and every fork-unverified dependency is **gated behind a spike**, never on the committed path.

---

## 1. Goal & non-goals

**Goal.** Re-architect Cartograph so that compute cost — CPU, GPU, RAM, VRAM, and network — is **bounded by what is visible and what changed this frame, not by factory size**. Concretely, at 1M+ buildings:

- Per-build-event CPU: **O(1)** amortized, *flat across a multi-hour session* (kills the #14 "worsens over ~1 h" signature, which is the redirector's per-event scan length growing with *cumulative-lifetime adds* — `problemDigest:mem-redirector-leak`, diagnosis §2).
- Initial load: **O(N)** streaming, interruptible (today: a single uninterruptible **O(N²)** game-thread stall — `problemDigest:redirector-on-add-on2-initial-load`).
- Host dataset RAM: from **~360–430 MB** of duplicated factory state (`problemDigest:mem-second-copy-currentbuildingdata`) to **~10–40 MB** of pure indices (engine state read live).
- VRAM: from a **fixed 256 MB** monolith (`problemDigest:target-8192-rgba8-256mb`) to a fixed ≤64–96 MB tile atlas → **~0 persistent** at the end-state, *independent of building count*.
- **Design out crash #14 (TDR):** no single GPU submission can approach the ~2 s Windows TDR window — structurally, by construction, not by tuning.
- **Design out crash #9 (dedicated-server join):** the entire hand-rolled slice-RCO bug class is *deleted*, not patched.

**Non-goals.** (1) No engine-source edits — everything is reachable from a GameFeature mod via SML hooks, public engine modules, and the in-repo `Plugins/`. (2) Not a feature expansion — the visible map output is preserved pixel-identically through the data-model migration; this is an efficiency and correctness re-architecture. (3) Not Iris replication — enabling Iris is a `Target.cs`/project-level toggle that rewrites the whole game's replication path and cannot be flipped by a GameFeature mod on CSS's shipped build; and the join problem is a one-shot bulk transfer, not high-frequency many-actor replication. (4) Not World-Partition-driven streaming — WP is read-only from a mod; the map's area-of-interest is the **UI viewport**, explicitly *not* the player's world-streaming AoI, and the two are never conflated.

---

## 2. Why the current design is unsalvageable as-is

**The index-redirector triad is the structural root, and it cannot be patched into health.** `CurrentBuildingData` is a `TArray<FBuildingData>` kept *globally Z-sorted* (only so the height slider can range-select via `Algo::LowerBound`), the spatial `TQuadTree<int32>` stores **positional indices into that sorted array**, and because keeping the array sorted shifts every later index on each insert, a `BuildingDataIndexRedirector` must re-base the quadtree's stale indices on **every single mutation** (diagnosis §3.1; `problemDigest:redirector-on-mutation`). This is an **O(N) scan per add and per remove** (`redirector-on-add-on2-initial-load`, `redirector-on-remove-on2-mass-dismantle`), an **O(N²) initial load** whose reindex branch is *provably never taken* during the monotonic sorted load yet still scans `M(M-1)/2` entries (verified in `problemDigest`), and a redirector that **never compacts** — its scan length equals *cumulative lifetime adds*, so per-event cost climbs across a session even when live count is flat. The three roles fatally conflated in one array — **storage**, **spatial culling**, and **Z-filtering** — are independent and must be decoupled. No amount of tuning the redirector fixes a structure whose central invariant (positions-as-identity) is the bug.

**A full second copy of the factory lives in RAM, half of it recomputable.** `FBuildingData` is ~344–432 B/building, of which **~152–192 B is precomputed draw cache** (`ScreenPosition`, `Size`, `Rotation`, `Corners[4]`, `VisualBoxCache`) — a pure function of `Transform` + per-class data, with the corner math computed *twice* (`FillInCache` and `FillInVisualBoxCache`) (`problemDigest:mem-second-copy-currentbuildingdata`, `mem-precomputed-datacache`). At 1M buildings this is ~360–430 MB duplicating `AFGBuildableSubsystem` / `AFGLightweightBuildableSubsystem` — the very arrays the gather reads (and copies **by value**, "Intentional copies", transiently doubling the factory at load — `initial-load-full-copy-by-value`). The wire format already proves the minimal record: `NetSerialize` ships only class hash + quantized position + compressed yaw and *deliberately drops* scale and the caches.

**The render path is a single unbounded GPU submission into a permanent 256 MB target.** `RENDER_TEXTURE_SIZE=8192` → a fixed **256 MB RGBA8** VRAM floor regardless of factory size (`target-8192-rgba8-256mb`), and the whole factory (~0.5–1M batched primitives) rasterizes in **one RHI submission** — `co_await Budget` paces only the game thread that *emits* elements; all batches flush at once (`gpu-one-pass-unbounded-tdr`). A megabase full redraw on one present interval exceeds the ~2 s Windows TDR → driver reset → **client crash #14**. Every redraw also issues a full-screen opaque 67 M-texel clear (`fullscreen-clear-every-redraw`), and the whole thing is amplified by a coroutine that is **cancel-restarted on every build event** — under a belt-drag it is a permanent cancel→restart loop producing *zero completed frames* until the player stops, re-paying the non-cancellable O(N) ingestion each restart (`cancel-restart-thrash-no-convergence`). The join path (`rco-null-deref-timeout-race`, `int16-sliceindex-overflow`, `sync-full-serialize-per-join-hitch`, `multicast-reliable-flood`) is a separate, equally unsalvageable hand-rolled transport.

---

## 3. Target architecture overview

The end-state is **server-authoritative**, **stable-handle**, **viewport-and-frame-bounded**. Identity is a generational handle, never an array position. Storage is an SoA column store keyed by that handle, with the *engine as the single source of truth* on any host (zero second copy). Spatial culling is a uniform fixed-cell hash grid keyed by the handle; Z-filtering is a separate coarse band index, decoupled from storage so the master columns never sort. Mutations are O(1) and only set dirty bits; a single never-cancelled compositor drains a monotonic dirty set under a frame-relative budget. The dataset reaches joining dedicated-clients as **per-tile, versioned, AoI-scoped deltas** over the in-repo `ReliableMessaging` transport — never a synchronous whole-dataset serialize. The render *primitive* is sequenced to de-risk (bounded tiled FCanvas → Slate instanced verts → gated GPU-instanced capture), but the data spine is identical under all three.

```
                    ┌──────────────────────── HOST / DEDICATED SERVER ───────────────────────┐
  SML build hooks   │                                                                        │
  AddBuildable ────►│  [O(1) MUTATION]                                                        │
  RemoveBuildable   │   patch handle slot ─► uniform grid (cells) ─► Z-band index             │
                    │                                  │                                      │
                    │                                  ▼  OR touched tile-id into             │
                    │                          DirtyTiles bitset (monotonic)                  │
                    │                                  │                                      │
   engine = truth   │   AFGBuildableSubsystem ─────────┤ (read by const-ref per dirty tile)   │
   (no 2nd copy)    │   AFGLightweightBuildableSubsys ─┘                                       │
                    │                                  │                                      │
                    │   per net-tick:  UE::Tasks worker packs slim blob + bumps tile version  │
                    │                  (snapshot on GT first; handles NotThreadSafe)           │
                    └──────────────┬───────────────────────────────────────────┬─────────────┘
                                   │ listen/SP: read indices directly           │ dedicated
                                   │ (NO network path)                          │
                                   ▼                                            ▼
                          ┌────────────────┐                    ReliableMessaging (chunk/ack/window)
                          │  COMPOSITOR    │                    delta-by-tile-version, AoI-ordered
                          │ never cancelled│                                    │
                          │ drains Dirty   │                                    ▼
                          │ tiles, frame-  │                          ┌──────────────────────┐
                          │ relative budget│                          │  DEDICATED CLIENT     │
                          └───────┬────────┘                          │  slim SoA (AoI only)  │
                                  │                                   │  + grid + Z-band      │
                                  ▼                                   │  LRU tile cache       │
                          ┌────────────────┐                         └──────────┬───────────┘
                          │  TILE / IMAGE  │  ◄──────────────────────────────────┘
                          │  Phase A: atlas│      (client renders locally from slim store)
                          │  Phase B: Slate│
                          │  Phase C: ortho│
                          │     capture    │
                          └───────┬────────┘
                                  ▼
                          Slate/UMG map UI (composites; viewport rect drives AoI pulls)
```

---

## 4. Subsystem deep-dives

### 4.1 Data model & spatial/Z indexing

**Identity — generational handle, never a position.** `FBuildingHandle{ uint32 Slot; uint16 Gen; }`. This single decision deletes the `(Z-sorted TArray + TQuadTree<int32> + BuildingDataIndexRedirector)` triad and with it root cause 1 in full: the O(N) reindex per mutation, the O(N²) load, and the never-compacting tombstone leak (diagnosis §3.1; `problemDigest:redirector-*`, `mem-redirector-leak`). The `Gen` counter makes handles ABA-safe so a recycled `TSparseArray`/`AbstractInstance` slot can never alias a stale network delta or UI pick — mandatory because slot reuse is exactly what makes the per-event cost O(1) (diagnosis §6 "TSparseArray slot reuse changes ID stability"; `hashgrid-vs-quadtree`).

**Storage — SoA / ECS column store, `TSparseArray`-backed.** Keyed by the handle, free-list reuse, **positions never shift** (O(1) `Add`/`RemoveAt`). Columns:

| Column | Type / size | Rationale |
|---|---|---|
| `Pos` | `FVector3f` (12 B) | single-precision is sufficient — ~0.05 cm worst-case error at the 7.5 km edge; `NetSerialize` already drops scale/quat, proving the minimal record |
| `Yaw` | packed `int16`/`float16` (2 B) | top-down needs only yaw — no quat, no scale |
| `ClassId` | `uint16` (2 B) | index into the per-class table (hundreds of classes) |
| `Type` | `uint8` | Icon / Rectangle / Spline / Wire / Beam |
| `ExtraIndex` | `uint32` | into **typed** side-tables (splines/wires/beams) so a belt never pays for a rectangle cache — fixes the `std::variant`-sized-by-largest waste (`mem-precomputed-datacache`) |

Everything that is a pure function of `Transform`+class is **deleted and recomputed at draw** via one shared pure `ComputeDrawGeometry(record, classInfo)` that must reproduce the old `FillInCache` math *pixel-identically* (extract once, diff-render a large save, then delete the stored fields — diagnosis §6). That removes ~152–192 B/building of recomputable cache and the duplicated corner math.

**Per-class table.** `TMap<uint16 ClassId, FClassDrawInfo> ClassDrawTable`, sized by *distinct classes* (hundreds), built once at gather: icon texture, footprint size, category color, layer, extra-rotation. The four per-building pointers the old design stored (`CategoryData`/`LayerData`/`SplineData`/`WireData`) collapse to one hash lookup per class at draw.

**Spatial index — uniform fixed-cell hash grid.** Bounds are `constexpr` (`WEST/EAST/NORTH/SOUTH` verified in `CartographGameInstanceModule.h`) and **anisotropic** (`MAP_WIDTH 750000.66` vs `MAP_HEIGHT 750000.00` cm — critic-flagged). Define the grid **in cm with per-axis `ceil` cell counts** and handle ragged edge tiles; do not assume square cells. A flat `TArray` of cells (≈256 m, tile-grid-aligned), each a small `TArray<FBuildingHandle>`. Insert = O(cells-covered) push by handle; remove = O(1) swap-remove by handle (no equal-Z `==` scan, no redirector walk); query = O(cells-in-rect + hits). Replaces the quadtree's 40 B/element `FBox2D` entirely (`mem-quadtree-and-countmap`). Multi-cell buildings (long belts, large foundations) store a small per-record cell list maintained consistently on move/remove.

**Z-filter — separate coarse band index.** ~64 fixed 1 m bands (`TArray<TArray<FBuildingHandle>>` or a per-cell band bitmask), **decoupled from storage** so the master columns never sort — this deletes the `operator<=>` Z-sort coupling that was the single decision forcing unstable positions (`z-sort-coupling`). The height slider becomes a band-range select intersected with the spatial query; `OnZFilterUpdated` goes from an O(N) full re-walk + 256 MB clear to **O(bands + hits)** (`client-full-rebuild-on-zfilter`).

**Before/after Big-O (this subsystem):**

| Operation | Before | After |
|---|---|---|
| Add | O(N) (insert memmove + redirector reindex) | **O(1)** amortized |
| Remove | O(N)×3 (equal-Z `==` scan + redirector walk + `RemoveAt` shift) | **O(1)** |
| Initial load | O(N²) | **O(N)** |
| Z-slider | O(N) + full clear | **O(bands + hits)** |
| Per-building RAM | 344–432 B | host ~8–12 B index; client ~24–28 B slim |

**Repo-verified refinement (render-side only, Phase C).** Do **not** hand-build a `TSparseArray` instance buffer + manual `StructuredBuffer` patching (architecture #1 over-engineers this). Back the *render* store with `AbstractInstance` (`Plugins/AbstractInstance`): `FInstanceOwnerHandlePtr` is the stable handle, `SetInstanceFromDataStatic`/`RemoveInstance` are O(1) with internal swap-patch, `SetCustomPrimitiveDataOnHandle` carries `worldZ`/`layerId`/`colorIndex`. **Verified caveats that shape this:** the handle is `TSharedPtr<…, ESPMode::NotThreadSafe>` (game-thread-only — so AbstractInstance mutation can never move to a worker), the manager builds `ULightweightCollisionComponent` by default (disable collision for a visual-only marker layer), and `AbstractInstance.Build.cs` lists **`AkAudio`/Wwise** as a dependency — so enabling it is *not* a one-line uncomment; it pulls a heavyweight audio plugin transitively (Phase 4 prerequisite, not a flag). The **CPU-side grid and Z-band index remain regardless of render path** — they are needed for picking, dirty-rect bookkeeping, and (on dedicated servers) slim-blob packing, none of which the GPU instance buffer provides.

### 4.2 Render pipeline (tiling / LOD / GPU-bounding / VRAM ceiling)

The render layer is the one real divergence among candidate designs. Resolution: keep the **tile / dirty-rect concept** for CPU-side culling, budgeted convergence, and per-tile network versioning, but **sequence the image backend to de-risk**. Ship A → promote to B → gate C. The tiles are a **mod-owned fixed grid over the constexpr bounds**, deliberately *decoupled from World Partition cells* (critic: WP cell-load/unload events are unverified-reachable from a mod, and WP cell size is an engine setting; coupling tile==cell is brittle). Tile pulls are driven purely by the **UI viewport rect**.

**Phase A — bounded per-tile FCanvas + scissored hardware clear (PRIMARY; the *only guaranteed* TDR fix).** This is Epic's officially documented TDR-avoidance pattern: *"each tile is submitted as a separate GPU command buffer."* A persistent `UCanvasRenderTarget2D`, **never fully cleared**, `bAutoGenerateMips=false` (else `EndDraw` kicks a whole-texture mip pass that silently defeats the scissor — `target-8192-rgba8-256mb`). Build events mark touched tiles in a `TBitArray`. Per frame a budgeted scheduler pops up to K dirty tiles; for each it issues a true scissored `RHIClearRenderTargetView` bounded to that tile's rect (~1 MB vs the current full-screen 67 M-texel opaque clear — `fullscreen-clear-every-redraw`), redraws only the buildings the grid reports in that tile (Z-band filtered), then `EndDrawCanvasToRenderTarget`. **Each tile is a separate GPU command-buffer submission**, so a megabase first-paint drains across `ceil(dirtyTiles/K)` frames and no submission approaches ~2 s — **crash #14 is structurally impossible.** Drop `RENDER_TEXTURE_SIZE` 8192→4096 (match the uasset `SizeX/Y`): **256 MB → 64 MB**, with `ORIGIN_UV`/`PIXEL_PER_CENTIMETER` (both `constexpr` off the constant) staying self-consistent.

> **BLOCKING Phase-A acceptance test (critic-elevated, fork-behavior-dependent).** Phase A's correctness hinges on the 2nd `BeginDrawCanvasToRenderTarget` on the persistent target **not** implicitly re-clearing — the branch-history "lines going crazy" note traces to exactly this. Test: draw tile A → `EndDraw` → draw tile B → `EndDraw` → read back and assert tile A's pixels survived. **If CSS's RHI implicitly clears on `BeginDraw`,** the concrete fallback — reachable today because `RHI` is already in `PrivateDependencyModuleNames` — is to bypass `UKismetRenderingLibrary` and issue clear+draw via a direct RDG/RHI `AddPass` with explicit `ERenderTargetLoadAction::ELoad` on the scissored rect. Document `ELoad`-via-RHI as the committed Phase-A fallback so a preserve-failure cannot strand the whole render plan.

**Phase B — custom Slate `SLeafWidget` + `FSlateDrawElement::MakeCustomVerts` (PREFERRED 2D default).** A custom leaf widget whose `OnPaint` emits a per-instance buffer — **one instanced draw batching all buildings** (a unit quad/line per building). Per Froyok's benchmark this is ~10× cheaper on the render thread than FCanvas-to-RT (~1.3 ms vs ~12 ms for equivalent work), it **deletes the persistent VRAM target entirely** (64 MB → ~0 persistent; the panel composites via Slate's own swap-chain pass), and it removes the **process-wide `FCanvas::GetBatchedElements` hook** by construction (the cross-mod `static_cast` hazard + per-batch tax flagged at `module.cpp:322/342`, `global-canvas-hook-overhead`). **Z-order:** `MakeCustomVerts` is not depth-sorted across instances, so bake draw order into *instance order* via the Z-band index (paint back-to-front — exactly what the Z-band index already produces). **Zoom-LOD:** re-expressed as instance-count culling to the viewport clip rect, replacing texture mips. UE 5.5+ async-Slate / parallel-RHI (~2×) is available on 5.6.1 and amplifies this, but tune the frame budget conservatively and validate.

> **Phase-B feasibility spike (critic-flagged; B is also the permanent fallback if C is blocked).** `Slate`/`SlateCore`/`UMG` are in `Build.cs` and `MakeCustomVerts` is upstream-public, but there is *zero* `MakeCustomVerts`/`SLeafWidget` usage in `Source/FactoryGame` to prove the symbol is exported on CSS's Slate build, and the Froyok numbers are from a non-mod compositing context. **Validate as a standalone spike** (one `SLeafWidget` drawing 100k custom-vert quads into the map UMG panel) before treating B as default. If it composites correctly through FG's UMG/HUD, adopt; if not, **Phase A remains the floor** — still ships the spine + the guaranteed TDR fix, just without the VRAM-to-zero and hook-removal wins. **A is the guaranteed floor; B's wins are contingent.**

**Phase C — GPU-instanced `AbstractInstance` + orthographic `USceneCaptureComponent2D` (GATED end-state).** One `ULightweightHISM` per class on a hidden, collision-disabled marker actor, captured top-down by an orthographic capture component. Per-instance custom data (`worldZ`, `layerId`, `colorIndex`) drives a material `clip()` for Z-band + layer filtering at **zero per-event CPU**; add/remove become O(1) handle ops. **Adopt only after** validating capture isolation (`PrimitiveRenderMode=UseShowOnlyList` + `ShowOnlyActors(MarkerActor)` + `GetViewOwner()` override) on the CSS renderer fork — the `Cartograph_2.0` POC used exactly this and **never compiled** (`branches.json`). Two corrections folded in from critics:

- **Stop citing `bEnableOrthographicTiling`/`NumXTiles`/`NumYTiles` as a structural TDR bound** in the net/complexity claims. These fields appear in *zero* C++ in the repo and there is no `Engine/` source to confirm them on the fork's capture component. Demote to: *"IF the field exists AND honors `NumXTiles` at runtime on the CSS fork."* The **only verified TDR bound is Phase A's per-tile separate-command-buffer EndDraw.** C is upside, not a second guarantee. (And ortho tiling requires `CaptureSource=SceneColor`, not FinalColor.)
- **ISM high-churn risk:** UE5 ISM `AddInstance`/`RemoveInstance` regressed for high-churn megabases (Chaos collision rebuild; community-reported 250k instances = 5 min on 5.7 vs 10 s on 4.27). Mitigate by disabling collision (default-on, verified) + `MarkDirtyDeferred` batching, and **benchmark game-thread-only handle-write throughput on a 1M cold load** before promoting C over B. Because `FInstanceOwnerHandlePtr` is `NotThreadSafe`, 1M handle ops are **unparallelizable** — a many-frame game-thread drain with no worker offload, unlike A/B which *do* allow worker POD packing.

**Rejected as primary — hand-rolled RDG/custom-shader rasterizer.** The Cartograph GameFeature is `LoadingPhase=Default` with no `Shaders/` dir; shipping global shaders needs a `PostConfigInit` shader module or the `ShaderDirectoryMapper` trick, and real rasterization touches renderer-private types not exported on a binary fork. Keep only as a profiled optional **Phase N** — and **do not promise it as a guaranteed escape hatch** (its own feasibility on a binary CSS fork is unproven). If A–C cannot hold frame time on an extreme megabase, the honest fallback is **aggressive LOD / instance-count culling within Phase B** (cull to viewport + zoom-decimate), not a custom rasterizer.

### 4.3 Async & frame-relative budget

**Convergence = monotonic accumulation, never cancel/restart** (resolves amplifier root cause 4 — `cancel-restart-thrash-no-convergence`).

1. **Build hooks do O(1) work only:** patch one column slot / one `AbstractInstance` handle, insert/erase the handle in grid + Z-band index, `OR` the touched tile id(s) into a monotonic `DirtyTiles` set. They enqueue nothing heavy and never touch the compositor. Because ingestion is now O(1), there is **no `FCancellationGuard`-protected O(N) block to re-pay**.
2. **One long-lived convergent compositor** runs every frame under a **frame-relative budget** — keep the vendored `UE5Coro` `FTickTimeBudget` model (a *fraction* of frame time, e.g. `r.Cartograph.FrameBudgetFraction=0.15`, **not** an absolute ms — the correct answer to issue #10). It is **never cancelled.** It drains the dirty set: pops up to K dirty tiles, renders them, and only ever *shrinks* the set — completed tiles stay valid; a tile re-dirtied mid-pass is coalesced by its bit (dirtied 100× → renders once). Critically, **await once per tile (or per K buildings), not per primitive** — this kills the ~9·N per-primitive awaiter + `FPlatformTime::Cycles()` reads (today it awaits after every rectangle edge and every spline segment — `per-primitive-budget-suspension-overhead`, `per-line-budget-yield-batch-thrash`).
3. **Off the game thread via `UE::Tasks::Launch`** (native on 5.6): per-dirty-cell POD snapshot packing and per-tile blob serialization/compression on workers. **Strictly game-thread-only:** reading the live engine buildable arrays (races concurrent builds — *snapshot on the game thread first*, hand the immutable POD copy to the worker), `AbstractInstance` handle mutation (`NotThreadSafe`), all render-target/Slate-vertex/RHI ops. Keep `UE5Coro` for the latent frame-budgeted draw drain; use `UE::Tasks` only for pure-CPU packing.
4. **Backpressure:** prioritize tiles in the current UI viewport AoI so the visible map converges first; coalesce mass-paste bursts into one render per tile. The explicit user-triggered full-redraw button (#10) just sets all populated tiles dirty.
5. **Dedicated server skips all draw bookkeeping** — no compositor, no tiles-to-pixels; it only maintains grid + Z-index + per-tile slim blobs and bumps versions on the game thread, with serialization on workers.

**Pre-resolve and PIN all distinct icon textures before any draw pass** so the loop never `co_await`s `AsyncLoadObject` mid-pass (current bug `sync-texture-load-mid-pass`, `module.cpp:758`).

### 4.4 Network transport & join

**Delete the hand-rolled slice RCO wholesale** — it is where crash #9 lives: three unchecked `*TMap::Find()` null-derefs racing the 10 s per-slice timeout (`rco-null-deref-timeout-race`), the `int16` slice-count overflow at ~63 MB (`int16-sliceindex-overflow`), the fixed-2047-byte stack-tail leak (`netserialize-fixed-2047-waste`), the per-join synchronous `Archive << CurrentBuildingData` on the game thread (`sync-full-serialize-per-join-hitch`), and the reliable `NetMulticast` per build event (`multicast-reliable-flood`). None survives.

**Transport = the in-repo `ReliableMessaging` plugin** (`Plugins/ReliableMessaging`): `SendTaggedMessage(FGameplayTag, TArray<uint8>)` over a chunked/acked/windowed transport. `SizeType=int32` and `MessageIdType=uint64`, so **the int16 overflow bug class is gone by construction**; per-`PlayerController` component with disconnect teardown kills the timeout-race null-deref and per-join leak by construction. `ReliableMessagingTCP` links on the dedicated-server target (no Server deny-list) where EOS/Steam P2P are denied — so the dedicated-server join path #9 is covered.

> **HIGHEST-LEVERAGE NETWORK SPIKE (critic CRITICAL — Phase 0 gates on this).** `ReliableMessaging` is a CSS-internal plugin and there is **no mod-wiring path visible in the repo**: nothing attaches `UReliableMessagingPlayerComponent` to a `PlayerController`, calls `SetTransportLayerConnection`, or drives the `Server_AdvertiseNewConnection` handshake — that wiring is CSS-internal/binary, and the component is `UCLASS(Within=PlayerController)` with a `GetFromPlayer()` accessor implying *the game already attaches it*. **Spike first, before any other network work:** on a live dedicated-server connection, call `GetFromPlayer(PC)` and verify (a) a component exists, (b) its transport reports `Connected` without the mod doing handshake work, and (c) the mod can `RegisterTaggedMessageHandler` for a *mod-owned* tag without colliding with CSS's fixed `FGReliableMessagingTags` enum. If it is *not* auto-attached, determine whether a GameFeature can `AddComponent` + drive the (protected) handshake RPCs. This is the single biggest "secretly needs the game's cooperation" risk in the whole design.

**Design = delta-by-tile-version / per-AoI lazy pull**, not reliable-multicast-per-event:

- **Versioning (critic-corrected):** the server bumps a **monotonic per-tile `uint64` counter, O(1) on dirty** — *not* a content hash on the hot path. Hashing the full content of every touched tile per change is residual per-event O(tile-occupancy) hidden by the complexity table; under a belt-drag/blueprint burst that is real CPU. Compute a content **hash lazily off-thread at re-serialize**, used only as a *client-side dedupe checksum*, never as the version identity. Reconnect/late-join determinism comes from the client comparing its last-seen counter per tile.
- **Join (fixes #9 by design):** the joining client sends its initial UI-viewport AoI; the server replies with a small **manifest** (tile list + version per tile), then streams the per-tile slim blobs for that AoI. No global serialize, no per-joiner full copy: the server holds **one shared immutable ref-counted snapshot per tile** (`TSharedRef<const TArray<uint8>>`) pinned across all joiners, so join memory is **O(joiners + AoI)**, not O(joiners·N). The client renders tiles progressively as they arrive.
- **Live deltas:** the server coalesces per-tile version bumps per net-tick and pushes, per client, only deltas for tiles in that client's AoI; far-away edits invalidate only their tile and never touch the client's current view.

**Verified `ReliableMessaging` caveats that shape the design:**

- `SendTaggedMessage` takes the payload **by value** — each tile blob is materialized at send. This *reinforces* the per-tile slim design (~28 B/record + RLE for empty tiles) and **forbids one whole-dataset blob.**
- **In-flight RAM amplification (critic):** `PayloadsPendingConnection` buffers full payloads until `Connected`; a first-join AoI burst transiently materializes the packed blob + the by-value send copy + the pending-connection copy + the chunker's `WriteBuffer` — up to ~3–4× the AoI bytes per joiner during handshake. **Mitigation:** complete the handshake (component reports `Connected`) *before* enqueuing the bulk manifest, and cap the per-joiner in-flight AoI to a small **N-deep ring** (refill on ack) so materialization stays bounded regardless of total AoI. The shared `TSharedRef<const>` keeps the *source* single-copy; the ring bounds the per-connection copies on top.
- **Strict-FIFO send queue (critic — bounded O(AoI) join not free):** the queue is not priority-sorted, so a big first-join blob blocks later AoI tiles; distance-order only orders the *initial* enqueue. **App-level priority** is required: order the manifest by distance from the viewport center, cap per-message blob size, keep the in-flight window 1–2 messages deep, and **load-test FIFO under simultaneous join + belt-drag** for head-of-line blocking.
- Use a **small fixed gameplay-tag set** (manifest / tile / version), not a tag-per-tile (tag registration costs a round-trip).

### 4.5 Server/client compute-&-storage partitioning

The explicit "compute once on the server, never recompute on the client at join or chunk-load" model. **Determinism caveat folded in (critic high):** the tile version is a canonical-byte ordering so reconnect is idempotent, and the design is **partitioned strictly by role** — a listen host has *no* network path at all, so its compositor reads indices directly and never serializes.

**What is computed-once-and-stored on the server** (authoritative; clients never recompute): the uniform grid + Z-band index, the `ClassDrawTable`, the per-tile version counters, and the per-tile slim instance blobs. These are **derived** from the engine subsystems by one O(N) streaming bucketing pass at world-load (no O(N²), no second copy), then kept incrementally correct by the O(1) build hooks. They are a **runtime cache**, deterministically reconstructable from authoritative engine state.

**Host = listen-server / single-player (server+client in one process).** Reads `AFGBuildableSubsystem::GetAllBuildablesRef()` / `AFGLightweightBuildableSubsystem::mBuildableClassToInstanceArray` as the **single source of truth** and keeps **zero second copy** — only the grid + Z-index + `ClassDrawTable` + (Phase C) the render-side `AbstractInstance` handles. Renders directly from engine transforms; **no slim store, no network path, zero serialization** (the by-value "Intentional copies" gather at `module.cpp:455` becomes const-ref — `initial-load-full-copy-by-value`). The map UI reads the host's own indices/render output. Same code with the transport stubbed.

> **Host-as-truth spatial-API caveat (critic CRITICAL).** The zero-second-copy host has **no mod-reachable random-access spatial API into the engine**: the lightweight octree is private, the public accessor is a *class-keyed `TMap`*, and `RemoveByInstanceIndex` renumbers indices. So "read live per dirty cell" naively pays O(N)/cell. **Resolution:** the host **does keep the slim ~24–28 B/building record as the spatial-query backing** (still no precomputed draw caches, still no full `FBuildingData` — ~26 MB @1M, not ~360 MB); it reads the engine arrays live *only at gather/rebuild* and on reconcile, and uses **generation-guarded `{class, lightweight-index}` keys** so engine-side renumbering can't alias a handle. This nuances the "host stores ~8–12 B of pure indices" claim upward to ~24–28 B/building on the host — still a 12–15× win, just not literally zero geometry. The *precomputed-cache* deletion and the *no-full-FBuildingData* win both stand.

**Dedicated server:** holds the authoritative indices + per-tile slim blobs + versions but **renders nothing** — no atlas, no Slate widget, no SceneCapture, no FCanvas (saves all VRAM and render-thread cost; today it pointlessly runs the full O(N²) ingestion and per-event multicast — `dedicated-server-does-render-bookkeeping`). It is the only place the authoritative derived state lives for thin clients.

**Dedicated-server client (the only config holding a per-client record copy):** lacks the subsystem data, so it keeps **only the slim SoA store** (~24–28 B/building) for its AoI tiles, fed by the `ReliableMessaging` tile stream, and renders locally. On map-pan/AoI-change it pulls newly-entered tiles and evicts left tiles (LRU); on a build event it gets a version ping and lazily re-pulls; on a WP chunk-load it does an **O(1) version check, never a recompute.**

**Explicit resource trade:** the server spends modest CPU (O(1) version bumps + off-thread repack of dirty tiles) + RAM (slim blobs, ~26–28 MB @1M) + bandwidth (changed AoI tiles + tiny version pings) so that **every client avoids recomputing a second full dataset**, joins are O(AoI) and crash-free, and chunk-loads are O(1). Cost moves from the many (clients, repeatedly) to the one (server, once per version).

### 4.6 Memory model

Peak RAM and VRAM are both **decoupled from runaway growth and flat across a build/dismantle session** — the explicit #14 success criterion (the "worsens over ~1 h" signature came from the never-compacting redirector growing the per-event scan with cumulative-lifetime adds; deleting it removes the signature — `mem-redirector-leak`).

**Deleted:** (a) the ~344–432 B/building `FBuildingData` second copy + ~152–192 B precomputed caches (`mem-second-copy-currentbuildingdata`, `mem-precomputed-datacache`); (b) the never-compacting redirector (the session-long O(cumulative-adds) leak); (c) the quadtree's 40 B/element `FBox2D` (`mem-quadtree-and-countmap`); (d) the in-RAM `Scale3D`+`FQuat` (top-down needs only yaw); (e) the by-value engine-map deep copy at load (`initial-load-full-copy-by-value`); (f) the per-disconnect RCO buffer/timer leak (`ReliableMessaging` owns buffers with disconnect teardown).

**Resident:** host = grid (~4–6 B/live building) + Z-band (~4 B) + slim record (~24–28 B for spatial backing, per §4.5 caveat) + `ClassDrawTable` (KBs) + (Phase C) the `AbstractInstance` buffer (~16–64 B/building, mostly VRAM). Dedicated client = slim SoA for AoI tiles only (a few thousand buildings in a viewport ≈ tens of KB–few MB; 1M-in-view is never resident). Dedicated server = slim blobs (~26–28 MB @1M) + indices; no atlas/VRAM.

**VRAM = a fixed ceiling independent of building count** (200k, 1M, or 50M cost the same — the image is viewport-sized, not factory-sized). Phase A: ~64–96 MB persistent atlas (scissored HW clears, no 256 MB opaque clear). Phase B: ~0 persistent (Slate swap-chain composite) + a ~16 B/building instance buffer pageable per-AoI.

**Persistence (recomputable-cache stance):** derived state is a **runtime cache**, reconciled in `PostLoadGame` against the live subsystem arrays. *Optionally* persist via `IFGSaveInterface` (`ShouldSave→true`) **only** the grid + slim records to skip the O(N) cold re-bucket on load — content-hash-validated, version-prefixed, with a clean fallback to rebuild from engine arrays when stale. **Do not embed tile pixel snapshots** (bloats the save, churns format for cheaply-recomputable data). Treat the save as a cache, never a correctness dependency — **default this OFF** unless cold-load time is shown to be a problem (open question Q10).

---

## 5. End-to-end data flow

**(1) Build event — O(1) hot path.** A buildable add/remove fires an SML `AddBuildable`/`RemoveBuildable` hook (the verified `SUBSCRIBE_UOBJECT_METHOD_AFTER` hook points). The hook does *only*: patch one column slot (or `AbstractInstance` handle via `SetInstanceFromDataStatic`/`RemoveInstance`, O(1) swap-patch) + insert/erase the stable handle in the grid cells it covers + insert/erase in the Z-band index + `OR` the touched tile id(s) into the monotonic `DirtyTiles` bitset. On the server it also bumps those tiles' `uint64` version. No O(N) work, no coroutine cancel, no canvas touch. (Replaces today's O(N) redirector reindex + O(N) equal-Z scan + `Cancel()`+restart.)

**(2) Server compute/store — off the hot path.** Once per quiescence/net-tick, a `UE::Tasks` worker packs the slim blob for each stale tile from a **game-thread snapshot of the mod's own slim columns** (snapshot first — workers can't read live engine arrays or touch `NotThreadSafe` handles; cost is O(Σ occupancy of dirty tiles), not free). The new immutable `TSharedRef` snapshot atomically replaces the old (game-thread pointer swap); an optional content hash is computed here as a client dedupe checksum. This is the single authoritative derive, shared across all joiners.

**(3) Stream to clients — dedicated only.** The server coalesces per-tile bumps per net-tick and, per client, enqueues `SendTaggedMessage` for AoI tiles whose version the client lacks, **ordered by distance from the client's UI-viewport center** (app-level, since the queue is FIFO), N-deep ring, window 1–2 messages. `ReliableMessaging` chunks/acks/windows over TCP/EOS/Steam. *On a listen host/SP this entire step is stubbed* — the client reads the host's indices directly.

**(4) Client tile cache — dedicated client.** The `RegisterTaggedMessageHandler` callback receives a completed tile blob (`FOnBulkDataReplicationPayloadReceived` delivers it by **rvalue-ref** — move, slightly better than by-value), validates the 1-byte protocol version + tile version, idempotently clear-then-applies the slim records into its local AoI SoA store + grid, and marks the tile dirty for local render. Tiles outside AoI are evicted LRU.

**(5) Render — client, or host in-process.** The never-cancelled compositor drains `DirtyTiles` under the frame-relative budget, prioritizing the current viewport. Per dirty tile it recomputes geometry via the shared `ComputeDrawGeometry` from the slim record + `ClassDrawTable` and emits it — Phase A: scissored HW clear + bounded `EndDraw` per tile (separate command buffer); Phase B: appends to the `MakeCustomVerts` instance buffer in Z-band order; Phase C: patches `AbstractInstance` custom data + triggers one debounced capture. The Slate/UMG UI composites.

**Join flow (fixes #9):** client opens map → sends initial viewport AoI → server replies with the manifest (tile ids + versions for that AoI) → server streams the shared per-tile snapshots in AoI order → client renders progressively. No global serialize, no per-joiner full copy, no int16 counter, no unchecked `Find()`, no game-thread `Archive << CurrentBuildingData`. Join memory O(joiners + AoI); join time = a few hundred bounded round-trips, not ~2000 RTTs + an O(N) synchronous serialize.

**Chunk-load flow (O(1)):** client enters a new region → for each newly-relevant tile it checks its cached version against the server's last-known (or requests a manifest delta) → fetches only blobs it lacks. Never a recompute. Generation-guarded keys + `PostLoadGame` reconcile tolerate engine lightweight-index renumbering and streamed-out regions. **Note:** tiles are mod-owned (not WP-coupled), so "chunk-load" here is a UI-AoI-pan event, not a WP cell event — there is no dependency on an unverified WP server hook.

---

## 6. Complexity & memory tables

**Complexity (before → after):**

| Operation | Before | After |
|---|---|---|
| Init load / gather | **O(N²)** synchronous (redirector reindex provably never taken yet scanned) + by-value deep copy | **O(N)** streaming bucketing, const-ref, interruptible |
| Single placement | O(N) (LowerBound O(log N) + Insert memmove O(N) + redirector reindex O(N)) | **O(1)** amortized |
| Drag-K (K events) | diverges — cancel→restart, zero completed frames; each restart re-pays O(N) ingestion | **O(K)** O(1) hooks + continuous convergence under frame budget |
| Dismantle-K | O(K·N) (each remove ×3 O(N) scans) | **O(K)** |
| Region redraw | O(N) full clear + repaint; localized path's union AABB balloons across cancels | **O(cells-in-rect + hits)**; dirty tiles drain over `ceil(/K)` frames |
| GPU submission / TDR | **one unbounded** ~0.5–1M-prim RHI submission > ~2 s TDR → crash #14 | `ceil(dirtyTiles/K)` bounded separate command buffers → **TDR impossible** (Phase A guaranteed) |
| Z-slider | O(N) re-walk + 256 MB clear | **O(bands + hits)** (Phase C: 2 material scalar writes) |
| Join (dedicated) | O(joiners·N) sync serialize; ~2000 RTTs; int16 overflow at ~63 MB; 3 unchecked `Find()` crash | **O(joiners + AoI)** lazy tile pull; few-hundred bounded RTTs; #9 impossible by construction |
| Live net update | reliable `NetMulticast` full arrays per event → channel flood | coalesced per-tile version delta to AoI clients only — **O(changed tiles)** |

**Memory / VRAM / bandwidth (before → after):**

| Metric | Before | After |
|---|---|---|
| Per-building resident record | ~344–432 B `FBuildingData` (quat+scale 96 B + ~152–192 B caches + 4 pointers) | Host ~28–40 B (slim + grid + Z-index); dedicated client ~24–28 B slim |
| Precomputed draw caches | ~152–192 B/building (recomputable; corner math computed twice) | **0 B** — recomputed at draw |
| Spatial index / building | quadtree ~40–48 B `FBox2D` + redirector 4 B/lifetime-placement (never compacts) | ~4–6 B grid + ~4 B Z-band; no tombstones |
| **Client RAM @200k** | ~72 MB second copy (+ growth) | host ~6–8 MB indices; dedicated client ~5–6 MB slim AoI working set |
| **Client RAM @1M** | ~360–430 MB second copy (+ redirector/quadtree growth + transient load doubling) | host ~26–40 MB; dedicated client ~26 MB (full-resident) / tens of KB–MB (viewport AoI) |
| **Server RAM @1M** | O(joiners·N) per-join `FBufferWriter` copies + per-disconnect leak | ~26–28 MB slim blobs; shared `TSharedRef` snapshots O(joiners + AoI) |
| VRAM | 8192² RGBA8 = fixed **256 MB**, permanent | Phase A ~64–96 MB fixed atlas; Phase B **~0 persistent** + ~16 B/building pageable buffer — factory-size-independent |
| Per-redraw clear bandwidth | ~256 MB full-screen opaque clear every redraw | ~1 MB scissored HW clear per dirty tile / none persistent (B) |
| Resident trend over a long session | monotonically rising → #14 "worsens over ~1 h → TDR" | **flat** (all structures track live count via O(1) swap-remove) |
| Join bandwidth | whole dataset, uncompressed-record, per joiner | AoI tiles only, ~28 B/record + RLE empty tiles, shared snapshot |

---

## 7. Modern tooling adopted

| Technique | Role | Availability on 5.6.1-CSS | Source |
|---|---|---|---|
| **`AbstractInstance` (`Plugins/AbstractInstance`)** | stable `FInstanceOwnerHandlePtr` handles, O(1) add/remove swap-patch, per-instance custom data (Phase C render store) | **in-repo**, but `Build.cs` dep is commented out *and* pulls `AkAudio`/Wwise transitively; handles `NotThreadSafe`; manager builds collision by default | docs.ficsit.app/…/AbstractInstance |
| **`ReliableMessaging` (`Plugins/ReliableMessaging`)** | join + delta transport; `int32` sizes / `uint64` ids kill the #9 bug class by construction; TCP links on dedicated server | **in-repo**; **but no mod-wiring path in repo** — must reuse the game's auto-attached `UReliableMessagingPlayerComponent` (spike first) | repo source (CSS-internal) |
| **`UE5Coro` + `FTickTimeBudget`** | frame-relative latent draw drain, await-per-tile | **vendored** in the mod | repo source |
| **`USceneCaptureComponent2D` orthographic capture** | Phase C GPU rasterization of the marker layer | available; isolation (`GetViewOwner`/`ShowOnlyActors`) **unverified on CSS fork** (POC never compiled) — gated | dev.epicgames.com/…/SceneCaptureComponent2D (5.6) |
| **Per-tile separate-command-buffer `EndDraw` (Epic TDR pattern)** | the **only guaranteed** TDR fix (Phase A) | reachable via public `UKismetRenderingLibrary::Begin/EndDrawCanvasToRenderTarget` (already called) | dev.epicgames.com/…/how-to-fix-a-gpu-driver-crash |
| **Scissored `RHIClearRenderTargetView` / RDG `AddPass`** | per-tile ~1 MB HW clear; `ELoad` fallback if persist-preserve fails | `RHI`/`RenderCore` already in `Build.cs` | froyok.fr/blog/2020-06-render-target-performances; RDG docs |
| **`FSlateDrawElement::MakeCustomVerts` + `SLeafWidget`** | Phase B instanced 2D draw, ~10× render-thread cost cut, deletes persistent VRAM target + global FCanvas hook | public Slate API in `Build.cs`; **symbol-export on CSS Slate unverified** — spike before defaulting | froyok.fr; UE Slate API docs |
| **`UE::Tasks::Launch`** | off-thread POD packing / blob serialization on immutable snapshots | native on 5.6 | dev.epicgames.com/…/tasks-systems |
| **`TSparseArray` / `TBitArray` / `FVector_NetQuantize`** | stable-slot store, dirty bitset, wire quantizers — proves the ~28 B record | core engine, version-agnostic | UE container/NetQuantize docs |
| **`IFGSaveInterface` (`ShouldSave`)** | *optional* grid+slim-record cache; default OFF | FG/SML; recomputable-cache, reconcile in `PostLoadGame` | docs.ficsit.app/…/Savegame |

**Rejected / dated, with reason:**

- **Hand-rolled RDG / global-shader rasterizer as primary** — `LoadingPhase=Default`, no `Shaders/` dir, renderer-private types not exported on a binary fork; correctly demoted to optional Phase N (and *not* promised as a guaranteed escape hatch).
- **`bEnableOrthographicTiling` as a structural TDR bound** — appears in zero repo C++, unverifiable on the fork; demoted to conditional upside, not a guarantee.
- **UE5 Iris** — project/`Target.cs`-level toggle a GameFeature cannot flip on CSS's shipped build; wrong tool for a one-shot bulk transfer.
- **RVT/SVT for the live map layer** — RVT is tuned for *static* geometry; the canonical in-game-map RVT author warns moving/changing geometry makes it "struggle to keep up." A factory under active build is exactly the bad case. At a fixed 64 MB atlas the bespoke residency is cheap enough that RVT is over-engineering.
- **Nanite for 2D markers** — anti-pattern for flat low-poly quads; adds overhead with zero benefit.
- **ISM/HISM as the *only* renderer** — UE5 high-churn `AddInstance`/`RemoveInstance` regression; kept only as gated Phase C with collision disabled + benchmarked, never as the sole path.
- **Storing `TSoftObjectPtr<AFGBuildable>` per record** (from `show_buildable_ui`) — a memory *regression*; resolve actors lazily for the single clicked building instead.

---

## 8. What to salvage from existing branches / plugins

- **`Cartograph_2.0` / `MapCaptureComponent2D`** — **direction validated → Phase C**, code rejected (does not compile: `Niagara->SetAsset()` stub, undeclared `Ism`, Niagara commented out in `Build.cs`; dual conflicting render paths; no instance cleanup on demolish — `branches.json`). **Concretely portable regardless of backend:** (1) `GetBuildingBoundingBox` via `GetCombinedClearanceBox()` + CDO-walking fallback (backend-agnostic footprint derivation, feeds the grid and `ClassDrawTable`); (2) the `GetViewOwner()` capture-isolation override (the minimal correct mechanism — but the thing that *never compiled*, so it is the Phase-C gate); (3) per-class instance batching (component count bounded by class, not instance). Its untouched 256 MB target and missing demolish cleanup are *not* salvage.
- **`ReliableMessaging`** — adopt as the entire network transport (§4.4). Kills the #9 bug class by construction; the spike (auto-attached component? transport `Connected` without mod handshake? mod-owned tag?) is the Phase-0 gate.
- **`AbstractInstance`** — adopt as the Phase-C render store (§4.1 refinement). Enables O(1) GPU mutation + per-instance custom-data Z/layer filter; gated on the Wwise-coupling check + collision-disable + the high-churn benchmark.
- **`partial_redraw`** — **design confirmation only**, do not merge (self-described WIP, two refactors behind, never solved clip-the-draw — the `cpp:417 TODO: Stencil` gap — `branches.json`). Confirms the **batched dirty-rect (`UpdateArea`/`RedrawArea`) model** and the **event-driven incremental spatial index** are the intended design — both of which the spine adopts. Its `TQuadTree<FBuildingData>` (by-value) is a memory *regression*. Heed its named regression: the dirty rect was **lost across cancel/restart** — moot here because the compositor is never cancelled.
- **`show_buildable_ui`** — **archaeology only.** Two takeaways: it proves the spatial index can serve fast point/region picks (the foundation for localized redraw), and the EnhancedInput map-open gating pattern is reusable for the #10 full-redraw button. Its per-building `TSoftObjectPtr` is a memory regression — do not port.

---

## 9. Migration roadmap

Phased, dependency-ordered; each phase de-risks the next and is independently shippable and measurable.

**Phase 0 — De-risk the crashes at the source (parallelizable).** *Gated by the `ReliableMessaging` wiring spike (§4.4).* Delete the slice RCO; route join + live updates through `SendTaggedMessage` with a fixed mod-owned tag set (manifest/tile/version), per-PC handler, handshake completed before bulk enqueue. Bulk-build any remaining index in one O(N) load pass. **Validates:** #9 gone (force the timeout/disconnect race on a >63 MB factory); load no longer hangs O(N²). **Metrics:** join wall-clock + bandwidth; no server access-violation under the disconnect race.

**Phase 1 — The shared spine (highest leverage; ship first after Phase 0).** Replace `CurrentBuildingData` with the `TSparseArray`-backed SoA store keyed by `FBuildingHandle`; replace quadtree+redirector with the per-axis uniform grid keyed by handle (O(1) insert/erase); add the separate Z-band index; delete the `operator<=>` coupling and the `OnZFilterUpdated` full re-walk. Rewrite `OnBuildingDataAdd`/`Remove` to O(1). On the host iterate the engine subsystems by const-ref (delete the second copy + by-value gather) — keeping the slim record as spatial backing per the §4.5 caveat. Build the `ClassDrawTable` once; extract `ComputeDrawGeometry`, **diff-render a large save to confirm pixel-identical**, then delete the stored caches. **Validates:** per-event O(N)→O(1); load O(N²)→O(N); **resident memory flat across a build/dismantle session** (the #14 signature gone); the `[-1]` OOB hazard gone. No render change required. **Metrics:** `stat memory`/`memreport` flat across a 5-min belt place/remove repro; profiled `OnBuildingDataAdd/Remove` sub-ms at 200k and *not rising* as the session ages.

**Phase 2 — Convergent compositor + bounded tiled FCanvas (designs out #14; guaranteed render floor).** *Gated by the persistent-RT preserve acceptance test (§4.2), with the `ELoad`-via-RHI fallback ready.* Replace cancel-restart with the never-cancelled convergent compositor draining `DirtyTiles` under `FTickTimeBudget`, **await once per tile**. Partition into mod-owned per-axis tiles; per frame pop K tiles, each a scissored `RHIClearRenderTargetView` + bounded `EndDraw` = a **separate GPU command buffer**. Persistent target, never fully cleared, `bAutoGenerateMips=off`. Drop `RENDER_TEXTURE_SIZE` 8192→4096. Move per-tile POD packing/serialization to `UE::Tasks` workers; pre-pin icon textures. On the dedicated server skip all draw bookkeeping; add per-tile `uint64` versions + shared `TSharedRef` snapshots feeding Phase 0's transport with delta-by-tile-version + AoI ordering. **Validates:** **#14 TDR impossible** (RenderDoc: no submission exceeds the per-frame budget; megabase first-paint drains over bounded frames); belt-drag converges continuously; VRAM 256→64 MB; join is O(AoI) tile pull. **Metrics:** GPU ms/submission, draw calls, frame time under drag, VRAM.

**Phase 3 — Promote the render primitive to Slate `MakeCustomVerts` (preferred default).** *Gated by the Phase-B spike (§4.2).* Custom `SLeafWidget` emitting `MakeCustomVerts` with a per-instance buffer (one instanced draw); bake Z-order into instance order via the Z-band index; re-express zoom-LOD as instance-count culling. Retain the tile/dirty-rect machinery for CPU culling + network versioning only; delete the global `FCanvas::GetBatchedElements` hook and the persistent target. **Validates:** render-thread cost ~12 ms → ~1.3 ms (Froyok-class); persistent VRAM ~0; no cross-mod canvas tax. **Metrics:** render-thread ms, persistent VRAM, hook removed. *If the spike fails, Phase A remains the permanent floor.*

**Phase 4 (GATED) — GPU-instanced `AbstractInstance` + orthographic capture.** *Prerequisites: capture-isolation validated on the fork; `AbstractInstance`/Wwise link confirmed; collision-disabled HISM path confirmed; high-churn handle-write throughput benchmarked on a 1M cold load within `FrameBudgetFraction`.* One `ULightweightHISM` per class on a hidden collision-disabled marker actor; `SetInstanceFromDataStatic`/`RemoveInstance` (O(1), game-thread-only, `MarkDirtyDeferred` batched); `worldZ`/`layerId`/`colorIndex` → material `clip()`; debounced orthographic capture *if* `bEnableOrthographicTiling` exists+honors at runtime on the fork. Behind a CVar; the Slate/canvas path stays the always-available fallback. **Validates:** O(1) per-event GPU mutation; Z/layer filter at zero CPU; fixed-cost capture. **Metrics:** per-event GPU ms, cold-load drain frames.

**Phase N (optional, profiled only).** Hand-rolled RDG/global-shader rasterizer — only if A–C provably cannot hold frame time on an extreme megabase, and *only after* confirming renderer-private RDG types are reachable on the fork. The honest fallback if N is infeasible is aggressive LOD/instance-count culling within Phase B, not a phantom final tier.

**Dependency graph:** Phase 0 (gated by the network spike) → Phase 1 (independent, ship first) → Phase 2 (needs Phase 1, gated by the preserve test) → Phase 3 (needs Phase 2, gated by the Slate spike) → Phase 4 (needs Phase 1+3 store, multi-gated). Phase 1 is the load-bearing change; Phases 0+1+2 alone resolve both crashes, the lag spikes, the leak, the load hang, and the bulk of the memory complaint on a guaranteed-buildable base.

---

## 10. Risks, open questions & fallbacks

| # | Risk / open question | Severity | Resolution / fallback |
|---|---|---|---|
| Q1 | **`ReliableMessaging` has no mod-wiring path in repo** — likely must reuse the game's auto-attached component | **CRITICAL** | Phase-0 connection-time spike (`GetFromPlayer` exists? `Connected` without mod handshake? mod-owned tag?). If not auto-attached, determine if a GameFeature can `AddComponent`+drive the protected handshake. **Gates all of Phase 0.** |
| Q2 | **Host has no random-access spatial engine API** (octree private; accessor class-keyed `TMap`; `RemoveByInstanceIndex` renumbers) | **CRITICAL** | Host keeps the slim ~24–28 B record as spatial backing; read engine live only at gather/rebuild/reconcile; generation-guarded `{class,index}` keys. (Nuances "zero-copy host" to "no *full-FBuildingData/no-cache* copy".) |
| Q3 | **Persistent-RT preserve across `EndDraw`** historically broke on this fork ("lines going crazy") | medium | **Blocking Phase-2 acceptance test** (draw A, EndDraw, draw B, EndDraw, read back A). Fallback: direct RHI/RDG `AddPass` with `ERenderTargetLoadAction::ELoad` — `RHI` already a dep. |
| Q4 | **Slate `MakeCustomVerts`/`SLeafWidget` symbol-export + UMG compositing** unverified on CSS Slate | medium | Phase-B spike (100k custom-vert quads into the map panel). If it fails, **Phase A is the permanent floor** (spine + TDR fix retained; lose VRAM-to-zero + hook removal). |
| Q5 | **Phase-C capture isolation** (`GetViewOwner`/`ShowOnlyActors`) — POC never compiled on this fork | medium | Validate on the fork before committing; if it fails, Phase B is the permanent default. C is upside, not a guarantee. |
| Q6 | **`AbstractInstance` pulls `AkAudio`/Wwise**; manager builds collision by default | low–med | Confirm transitive-only link + that a no-Ak-geometry manager is a no-op; confirm a collision-disabled HISM path; else use a thin custom marker component. Keeps Phase 4 gated. |
| Q7 | **`AbstractInstance` high-churn cost** + `NotThreadSafe` handles = unparallelizable 1M cold load | low | Disable collision + `MarkDirtyDeferred`; benchmark game-thread handle-write throughput within `FrameBudgetFraction` before promoting C over B. A/B retain worker-thread POD packing. |
| Q8 | **Content-hash versioning is residual per-event O(tile-occupancy)** | high | Version = monotonic `uint64` counter, O(1) on dirty; hash only off-thread at re-serialize as a client dedupe checksum, never the version identity. |
| Q9 | **FIFO head-of-line + by-value/pending-connection RAM amplification** on first-join AoI burst | high | App-level distance-ordered priority; cap blob size; in-flight window 1–2 + N-deep ring; handshake before bulk enqueue; load-test FIFO under join+belt-drag. |
| Q10 | **WP cell-load/unload events + cell granularity** unverified-reachable from a mod | medium | **Decouple tiles from WP** — mod-owned fixed grid over constexpr bounds; tile pulls driven by the UI viewport rect; WP read-only for prioritization. Makes "O(1) chunk-load" a non-requirement. |
| Q11 | **Lightweight-index renumbering** can alias a handle | low | Generation-guarded `{class,index}` keys + `PostLoadGame` reconcile; validate against a real engine-side mass removal. |
| Q12 | **`ComputeDrawGeometry` pixel-identity** vs old `FillInCache` | low | Diff-render harness over a large save during Phase 1 *before* deleting the stored caches. |
| Q13 | **`FVector3f` single-precision forbids exact-equality transform compares** (the old value-quadtree identity) | low | Identity is the handle now; audit for and remove residual exact-equality position compares before switching. |
| Q14 | **Save-persistence policy** | low | Default OFF; ship the optional grid+slim-record cache (content-hash-validated, reconciled) only if cold-load time is shown to be a problem. Never embed tile pixels. |

---

## Bottom line

Cartograph's crashes and resource blowups are not a collection of bugs to patch but the symptoms of one structural decision — using **positional array indices as identity** — compounded by a duplicated factory in RAM and an unbounded single-pass GPU submission. The end-state inverts all three: a **stable generational handle** over an SoA column store with the engine as truth (host RAM ~360–430 MB → tens of MB, per-event O(N)→O(1), load O(N²)→O(N), the session-long leak gone), a **bounded tiled render** whose separate-command-buffer submissions make the TDR crash #14 *structurally impossible* and whose VRAM is a fixed ceiling independent of factory size, and a **versioned per-AoI delta stream** over the in-repo `ReliableMessaging` that deletes the entire #9 crash class by construction. The whole thing is an **integration project, not a research bet** — every primitive already ships in this repo's `Plugins/` or `Source/` and runs on the verified 5.6.1-CSS fork without engine edits. The committed path (Phases 0–2: spine + bounded tiled FCanvas + ReliableMessaging join) is high-confidence buildable today and resolves both crashes, the lag spikes, the leak, the load hang, and most of the memory complaint; the render upside (Slate instanced verts, then gated GPU capture) and every fork-unverified dependency are sequenced behind explicit de-risking spikes, so no committed phase ever rests on an unproven assumption. Ship the spine first.

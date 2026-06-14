# Cartograph Re-Architecture — Implementation Contracts

> **Status:** FROZEN shared contracts for the ground-up re-architecture. These headers + tunables are the compile surface every other agent builds against. Public signatures here are **stable** — downstream impl agents implement against them and MUST NOT change them. If a contract is insufficient, implement as best you can against it and report the deviation; do not edit a header you do not own.
>
> Authoritative design: `STAGE2-rearchitecture.md` (the SPEC). Diagnosis: `STAGE1-diagnosis-report.md`. Open questions referenced as Q1–Q14 live in SPEC §10.

---

## 1. File tree (new layout)

```
Source/Cartograph/
├── Cartograph.Build.cs                         (existing — add new deps when impl needs them)
├── Public/
│   ├── CartographConfig.h                       [contracts] CVars, tunables, world bounds, render-backend enum
│   ├── Core/
│   │   ├── CartographTypes.h                     [contracts] FROZEN shared data: FBuildingHandle, FBuildingRecord,
│   │   │                                                     FClassDrawInfo, FDrawGeometry, side-tables, coord helpers
│   │   ├── CartographBuildingStore.h             [contracts→store]      SoA store, generational handles
│   │   ├── CartographSpatialGrid.h               [contracts→grid]       uniform fixed-cell hash grid
│   │   ├── CartographZBandIndex.h                [contracts→zband]      coarse Z-band index (decoupled)
│   │   ├── CartographClassDrawTable.h            [contracts→classtable] per-class table + ComputeDrawGeometry
│   │   └── CartographTileManager.h               [contracts→tilemanager]dirty bitset + per-tile versions + AoI
│   ├── Render/
│   │   ├── CartographCompositor.h                [contracts→compositor] never-cancelled tiled FCanvas (Phase A, default)
│   │   ├── SCartographMapView.h                  [contracts→slate]      SLeafWidget + MakeCustomVerts (Phase B, gated)
│   │   └── CartographInstanceRenderer.h          [contracts→instance]   AbstractInstance + ortho capture (Phase C, gated)
│   └── Net/
│       └── CartographMapReplicator.h             [contracts→net]        per-tile versioned AoI delta over ReliableMessaging
└── Private/
    ├── CartographConfig.cpp                       [contracts] CVar definitions
    ├── Core/      CartographBuildingStore.cpp, CartographSpatialGrid.cpp,
    │              CartographZBandIndex.cpp, CartographClassDrawTable.cpp, CartographTileManager.cpp   [impl agents]
    ├── Render/    CartographCompositor.cpp, SCartographMapView.cpp, CartographInstanceRenderer.cpp    [impl agents]
    └── Net/       CartographMapReplicator.cpp                                                          [impl agents]
```

`[contracts]` = owned and frozen by the contracts agent (this manifest). `[contracts→X]` = header frozen by contracts; the matching `.cpp` is owned by impl agent `X`.

---

## 2. Tunables & CVars (`CartographConfig.h` / `.cpp`)

**World bounds (verbatim from legacy, anisotropic):** `WEST/EAST/NORTH/SOUTH_BOUND_CENTIMETERS`, `MAP_WIDTH_CENTIMETERS` (750000.664), `MAP_HEIGHT_CENTIMETERS` (750000.0).

**Render-target geometry:** `RENDER_TEXTURE_SIZE = 4096` (was 8192; **uasset SizeX/Y must be edited to 4096**), `ORIGIN_UV[]`, `PIXEL_PER_CENTIMETER[]` (derived constexpr, self-consistent).

**Tile grid:** `TILE_SIZE_PX = 256`, `TILES_PER_AXIS = ceil(4096/256) = 16`, `TILE_COUNT = 256`.

**Spatial grid:** `GRID_CELL_SIZE_CM = 25600` (~256 m), `GRID_CELLS_X/Y` (per-axis ceil), `GRID_CELL_COUNT`.

**Z-band:** `Z_BAND_COUNT = 64`, `Z_BAND_MIN_CM = -100000`, `Z_BAND_MAX_CM = 540000`, `Z_BAND_HEIGHT_CM` (~100 m/band).

**Draw tunables (verbatim from legacy):** `SPLINE_SEGMENTS = 8`, `BOX_EXPANSION_CENTIMETERS = 300`.

**Render backend enum:** `ECartographRenderBackend { TiledCanvas=0, SlateInstanced=1, InstancedCapture=2 }`. Read via `CartographConfig::GetRenderBackend()` (clamps to TiledCanvas on out-of-range).

**CVars:**

| CVar | Type | Default | Purpose |
|---|---|---|---|
| `r.Cartograph.RenderBackend` | int | 0 | `ECartographRenderBackend` selector |
| `r.Cartograph.FrameBudgetFraction` | float | 0.15 | compositor frame-relative budget (FTickTimeBudget) |
| `r.Cartograph.TilesPerFrame` | int | 8 | hard cap K on tiles drained per tick |
| `r.Cartograph.PersistSpatialCache` | bool | false | persist grid+slim records (default OFF, Q14) |

CVar externs are declared in `CartographConfig.h`, defined once in `CartographConfig.cpp`.

---

## 3. Frozen shared types (`Core/CartographTypes.h`)

| Type | Shape | Notes |
|---|---|---|
| `FBuildingHandle` | `uint32 Slot; uint16 Gen` | generational, ABA-safe; `IsValid()`, `==`, `GetTypeHash`. Slot validity vs store checked by `Store::IsValid`. |
| `EBuildingDrawType` | `uint8` enum: Invalid/Icon/Rectangle/Spline/Wire/Beam | single value per record (not bit flags) |
| `FClassDrawInfo` | per-class draw data | DrawType, Icon (soft), Footprint, MainColor/OutlineColor/OutlineThickness, StrokeThickness/StrokeColor, ExtraYawDegrees, LayerMain/SubCategory |
| `FSplineExtra` / `FWireExtra` / `FBeamExtra` | typed side-tables | `TArray<FVector2f> Points` / `FVector2f End` / `float Length` (single-precision) |
| `FBuildingRecord` | slim record (~24-28 B) | `FVector3f Pos; uint16 PackedYaw; uint16 ClassId; EBuildingDrawType Type; uint32 ExtraIndex`. `PackYaw`/`GetYawDegrees` helpers. |
| `FDrawGeometry` | ComputeDrawGeometry output | screen pos/size/rotation, `Corners[4]`, spline points, wire/beam end, colors, `FBox2f VisualBox`. Consumed by ALL backends. |
| `FTileId` / `FTileVersion` | `uint32` / `uint64` | `InvalidTileId` sentinel |

**`CartographCoords` namespace (frozen coord math):** `WorldToScreen` (with/without footprint), `ScreenToWorld`, `TileId`/`TileXY`, `TileIdFromScreen`, `ScreenRectFromTile` (ragged-edge-clamped), `GridCellFromWorld`, `ZBandFromWorldZ`. Ported from legacy `world_position_to_screen_position` / `screen_position_to_world_position` for pixel-identity (Q12).

---

## 4. Subsystem public APIs (frozen surface; bodies owned by impl agents)

### `FCartographBuildingStore` (Core) — agent `store`
- `Add(record) -> FBuildingHandle` **O(1)** amortized · `Remove(handle) -> bool` **O(1)** (bumps gen) · `IsValid` · `Get`/`Find` (mut+const) **O(1)** · `Num` · `Reserve` · `Empty`
- Typed side-tables: `Add{Spline,Wire,Beam}Extra -> uint32`, `Get{...}Extra`, `Remove{...}Extra` (all O(1))
- `ForEach(visitor)` over live records (slot order; Z-order comes from the Z-band index, not here)
- Replaces legacy `CurrentBuildingData` + `BuildingDataIndexRedirector`. **Never sorts.**

### `FCartographSpatialGrid` (Core) — agent `grid`
- `Initialize()` · `Insert(handle, FBox2f worldBox)` **O(cells)** · `Remove(handle, FBox2f worldBox)` **O(cells)** (pass same box) · `QueryRect(box, OutHandles)` / visitor form **O(cells+hits)** · `NumEntries` · `Reset`
- World-cm coordinates. Multi-cell buildings inserted into every covered cell. Query may yield duplicates + false positives (caller refines vs record VisualBox). Replaces `TQuadTree<int32>`.

### `FCartographZBandIndex` (Core) — agent `zband`
- `Initialize()` · `Insert(handle, float worldZ)` **O(1)** · `Remove(handle, float worldZ)` **O(1)** (pass same Z) · `QueryBandRange(minZ,maxZ, visitor|OutHandles)` **O(bands+hits)**, low→high (painters' order) · `NumEntries` · `Reset`
- Decoupled from store — deletes legacy `operator<=>` Z-sort coupling. Replaces `OnZFilterUpdated` O(N) re-walk.

### `FCartographClassDrawTable` (Core) — agent `classtable`
- `Build()` once **O(distinct classes)** · `GetClassId(UClass*) -> uint16` (`InvalidClassId=0xFFFF`) · `GetInfo(id) -> const FClassDrawInfo&` · `FindInfo` · `NumClasses` · `GatherIconAssets(OutIcons)`
- **`static ComputeDrawGeometry(record, info, store, OutGeometry) -> bool`** — the shared pure draw math. **MUST reproduce legacy `FillInCache`+`FillInVisualBoxCache` pixel-identically** (Q12). Pure/thread-safe on an immutable snapshot. O(1) except O(SPLINE_SEGMENTS) for splines.

### `FCartographTileManager` (Core) — agent `tilemanager`
- Dirty: `Initialize` · `MarkDirtyForBox(screenBox)` **O(tiles)** · `MarkDirty(tile)` · `MarkAllDirty` · `PopDirtyTiles(K, Out) -> int32` (AoI-ordered, monotonic shrink) · `DirtyNum` · `HasDirty`
- Version (server): `BumpVersion(tile)` **O(1)** · `GetVersion(tile) -> uint64` (monotonic counter, **not** a content hash — Q8)
- AoI: `SetAoI(screenBox)` · `GetAoITiles` · `GetTilesForBox` (static) · `TileRect`/`TileAtScreen` (static)

### `FCartographCompositor` (Render, Phase A / default) — agent `compositor`
- `Initialize(store, grid, zbands, classTable, tileManager, renderTarget)` (non-owning) · `Shutdown` · `GetRenderTarget`
- **`TickConverge()` — UE5Coro, NEVER cancelled**, drains K tiles/tick under `FTickTimeBudget`, await per-tile (not per-primitive). Started once.
- `RenderTile(tile)` — scissored HW clear + bounded `EndDraw` = separate GPU command buffer (the guaranteed TDR fix). · `SetFullRedraw` (marks all dirty; drains over frames) · `SetZFilter(min,max)` · `SetShowBuildings(bool)`

### `SCartographMapView` (Render, Phase B / gated) — agent `slate`
- `SLeafWidget`. `Construct(args, store, grid, zbands, classTable)` · `OnPaint` emits one `MakeCustomVerts` instanced draw (Z-band-ordered instances) · `ComputeDesiredSize` · `SetViewportWorldRect` · `SetZFilter` · `SetShowBuildings`
- **SPIKE(Q4):** symbol export + UMG compositing unverified on CSS Slate. TiledCanvas is the fallback.

### `UCartographInstanceRenderer` (Render, Phase C / gated) — agent `instance`
- `UObject`. `Initialize(store, classTable, world)` · `Shutdown` · `AddInstance`/`RemoveInstance`/`UpdateInstance` (**O(1), game-thread-only**, handles `NotThreadSafe`) · `SetZFilter` (2 material scalar writes) · `SetLayerVisible` · `SetShowBuildings` · `RequestCapture` · `GetCaptureTarget`
- **SPIKE(Q5/Q6/Q7):** capture isolation, AbstractInstance/Wwise link, ortho-tiling field, high-churn cost — all unverified. TiledCanvas/Slate is the fallback.

### Net — agent `net`
- **`FCartographMapReplicator`** (server, plain C++): `Initialize(store, grid, tileManager)` · `RepackTile(tile)` (game-thread snapshot → worker serialize → GT pointer swap) · `GetTileSnapshot(tile) -> TSharedRef<const FTileSnapshot>` (shared immutable, O(joiners+AoI)) · `BuildManifest(viewportBox, OutTiles, OutVersions)` (distance-ordered)
  - `FTileSnapshot { FTileVersion Version; TSharedRef<const TArray<uint8>> Blob }`
- **`CartographNetTags`**: `Manifest()`, `Tile()`, `Version()` — fixed mod-owned tag set (not tag-per-tile)
- **`UCartographMapReplicationComponent`** (`UCLASS(Within=PlayerController)`): `InitializeTransport` (reuses `UReliableMessagingPlayerComponent::GetFromPlayer`, **SPIKE Q1**) · client: `RequestAoI`, `OnTileReceived`, `OnManifestReceived`, `OnVersionReceived` · server: `BindServerReplicator`, `OnAoIRequested`, `PushPendingDeltas`
- Replaces `UCartographRemoteCallObject` wholesale. Transport facts honored: `SendTaggedMessage` by value; handler delivers payload by rvalue-ref; int32 sizes / uint64 ids (no int16 overflow); per-PC disconnect teardown. Listen-host/SP path is INERT.

---

## 5. Dependency DAG

```
                       CartographConfig.h
                              │ (bounds, tunables, CVars, render-backend enum)
                              ▼
                       Core/CartographTypes.h
        ┌──────────────┬──────┴───────┬───────────────┬──────────────┐
        ▼              ▼              ▼               ▼              ▼
  BuildingStore   SpatialGrid    ZBandIndex     TileManager   ClassDrawTable
        │              │              │               │         (ComputeDrawGeometry
        │              │              │               │          needs BuildingStore
        │              │              │               │          for side-tables)
        └──────────────┴──────────────┴───────────────┴──────┬────┘
                                                              │
                 ┌────────────────────────────────────────────┼────────────────────────┐
                 ▼ (Phase A, default)                          ▼ (Phase B/C, gated)      ▼
        Render/CartographCompositor          Render/SCartographMapView          Net/CartographMapReplicator
        (store+grid+zband+classtable+tile)   Render/CartographInstanceRenderer  (store+grid+tile)
                                              (store+grid+zband+classtable)      reuses ReliableMessaging
```

**Build order:** `CartographConfig.h` → `CartographTypes.h` → {`BuildingStore`, `SpatialGrid`, `ZBandIndex`, `TileManager`} (independent of each other) → `ClassDrawTable` (depends on `BuildingStore` for side-table resolution in `ComputeDrawGeometry`) → {`Compositor`, `SCartographMapView`, `InstanceRenderer`, `MapReplicator`} (each depends on the Core spine; mutually independent).

**Maps to SPEC phases:** Phase 1 = Core spine. Phase 2 = Compositor. Phase 3 = SCartographMapView (gated). Phase 4 = InstanceRenderer (gated). Phase 0 = MapReplicator (gated on Q1 spike).

---

## 6. Ownership map

| Files | Owner agent | Permission for others |
|---|---|---|
| `CartographConfig.h`, `CartographConfig.cpp` | **contracts** | read-only |
| `Core/CartographTypes.h` | **contracts** | read-only |
| `Core/CartographBuildingStore.h` | **contracts** (frozen) | impl in `CartographBuildingStore.cpp` (agent `store`) |
| `Core/CartographSpatialGrid.h` | **contracts** (frozen) | impl in `CartographSpatialGrid.cpp` (agent `grid`) |
| `Core/CartographZBandIndex.h` | **contracts** (frozen) | impl in `CartographZBandIndex.cpp` (agent `zband`) |
| `Core/CartographClassDrawTable.h` | **contracts** (frozen) | impl in `CartographClassDrawTable.cpp` (agent `classtable`) |
| `Core/CartographTileManager.h` | **contracts** (frozen) | impl in `CartographTileManager.cpp` (agent `tilemanager`) |
| `Render/CartographCompositor.h` | **contracts** (frozen) | impl in `CartographCompositor.cpp` (agent `compositor`) |
| `Render/SCartographMapView.h` | **contracts** (frozen) | impl in `SCartographMapView.cpp` (agent `slate`) |
| `Render/CartographInstanceRenderer.h` | **contracts** (frozen) | impl in `CartographInstanceRenderer.cpp` (agent `instance`) |
| `Net/CartographMapReplicator.h` | **contracts** (frozen) | impl in `CartographMapReplicator.cpp` (agent `net`) |
| `IMPLEMENTATION-CONTRACTS.md` | **contracts** | read-only |

**Read-only contract inputs (never edit):** the legacy `CartographGameInstanceModule.h/.cpp`, `CartographDataStructure.h/.cpp`, `CartographRemoteCallObject.h/.cpp`, `Util/CartographCanvasRenderItem.*`, and the `Plugins/ReliableMessaging` + `Plugins/AbstractInstance` headers.

**Frozen-header rule:** impl agents implement against the header as-is. If a contract is wrong or insufficient, implement as best you can and report it in `contractDeviations`; do NOT edit a header you don't own.

---

## 7. Notes for impl agents

- **`CARTOGRAPH_API` exports:** every public class above carries `CARTOGRAPH_API` so cross-TU use links on the monolithic mod module.
- **`ComputeDrawGeometry` is the pixel-identity gate (Q12):** port the legacy `FillInCache` (rect corners at ±half-footprint·scale, yaw-only transform with scale removed, project to screen; icon/spline/wire/beam paths) and `FillInVisualBoxCache` (union + expand by `BOX_EXPANSION_CENTIMETERS`) exactly, then diff-render a large save before deleting any legacy cache. The contract returns `bool` to mirror the legacy "not drawable" early-outs (zero footprint / zero stroke thickness).
- **Yaw packing:** `FBuildingRecord::PackYaw`/`GetYawDegrees` use a uint16 0..360° quantization. If exact wire compatibility with the legacy `SerializeCompressedShort` rotator is required, the net agent should pack/unpack at the wire boundary and report any precision delta in `contractDeviations`.
- **`Build()` no-arg form (classtable):** the no-arg `Build()` is the frozen contract. If the impl needs the legacy config maps passed in, add a parameterized overload in the `.cpp`/a private helper — do not remove or change the no-arg signature.
- **Thread-safety boundaries (SPEC 4.3):** `ComputeDrawGeometry` is pure and worker-safe on an immutable snapshot. `AbstractInstance` handle mutation, RHI/Slate/render-target ops, and reading live engine arrays are game-thread-only. `UE::Tasks` workers only touch POD snapshots.
- **Listen-host/SP:** the net path is inert; the compositor reads the indices directly (SPEC 4.5). Impl agents must keep the host code path free of any serialize call.
```

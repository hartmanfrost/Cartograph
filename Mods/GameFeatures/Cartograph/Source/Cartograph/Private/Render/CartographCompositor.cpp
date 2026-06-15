#include "Render/CartographCompositor.h"

#include "CartographConfig.h"
#include "Core/CartographBuildingStore.h"
#include "Core/CartographSpatialGrid.h"
#include "Core/CartographZBandIndex.h"
#include "Core/CartographClassDrawTable.h"
#include "Core/CartographTileManager.h"

// CARTO_LOG / CARTO_LOG_ERROR macros + LogCartograph (matches every sibling new
// subsystem .cpp). NOTE: this header ALSO re-declares the world-bounds / RENDER_
// TEXTURE_SIZE constexpr globals that CartographConfig.h supersedes (with a
// DIFFERENT RENDER_TEXTURE_SIZE: 8192 vs 4096) - an ODR/redefinition conflict in
// any TU that includes both (this one does, via CartographTypes.h -> Config.h).
// Reported in contractDeviations; the integration owner must guard/migrate the
// legacy constants. Logging-only dependency here.
#include "CartographGameInstanceModule.h"

#include "Algo/StableSort.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Kismet/KismetRenderingLibrary.h"

#include "UE5Coro/UE5Coro.h"

// RHI / RenderCore for the ELoad-via-RHI scissored-clear fallback (SPEC Q3).
// RHI is already in PrivateDependencyModuleNames, RenderCore in Public.
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "ClearQuad.h"  // DrawClearQuad - SPIKE(Q3): confirm exported on the CSS RenderCore

// =============================================================================
// CartographCompositor.cpp - Phase-A bounded tiled FCanvas (the TDR fix).
// SPEC 4.2 Phase A + 4.3. See the header for the architectural contract.
//
// The whole point of this file: NO single GPU submission may approach the ~2 s
// Windows TDR window. We guarantee that structurally by drawing exactly ONE
// tile per Begin/EndDrawCanvasToRenderTarget, so each tile is a SEPARATE GPU
// command-buffer submission, and a megabase first-paint drains across
// ceil(dirtyTiles / K) frames under a frame-relative budget. The compositor is
// NEVER cancelled (kills the legacy cancel/restart thrash) and the dirty set
// only ever shrinks during a pass.
//
// FDrawGeometry coming out of FCartographClassDrawTable::ComputeDrawGeometry is
// ALREADY in atlas PIXEL space (ScreenPos/Size/Corners/SplinePointsScreen px),
// so this file does NO cm->px conversion - it just emits FCanvas items. That is
// the one substantive difference from the legacy RedrawMapCoroutine, which
// stored Size in cm and multiplied by PIXEL_PER_CENTIMETER at draw.
// =============================================================================


void FCartographCompositor::Initialize(
	FCartographBuildingStore* InStore,
	FCartographSpatialGrid* InGrid,
	FCartographZBandIndex* InZBands,
	FCartographClassDrawTable* InClassTable,
	FCartographTileManager* InTileManager,
	UCanvasRenderTarget2D* InRenderTarget)
{
	Store = InStore;
	Grid = InGrid;
	ZBands = InZBands;
	ClassTable = InClassTable;
	TileManager = InTileManager;
	RenderTarget = InRenderTarget;

	if (!IsReady())
	{
		CARTO_LOG_ERROR("FCartographCompositor::Initialize called with a null dependency or render target");
		return;
	}

	// The persistent atlas must NEVER auto-generate mips: an EndDraw mip pass
	// rasterizes the WHOLE texture and silently defeats the per-tile scissor,
	// reintroducing the unbounded submission this whole phase exists to kill
	// (SPEC 4.2, target-8192-rgba8-256mb). The owning subsystem created the RT;
	// we enforce the invariant defensively here.
	if (UCanvasRenderTarget2D* RT = RenderTarget.Get())
	{
		RT->bAutoGenerateMips = false;
		// SPIKE(Q3): the persistent target MUST preserve its contents across the
		// 2nd+ BeginDrawCanvasToRenderTarget (we never fully clear it; only the
		// per-tile rect is cleared each pass). On stock UE this holds because the
		// canvas RT keeps its resource between draws, but the CSS-fork branch
		// history ("lines going crazy" / implicit re-clear on BeginDraw) means
		// this must be confirmed with the Phase-2 acceptance test (draw A,
		// EndDraw, draw B, EndDraw, read back A). If it FAILS, switch the per-tile
		// clear in DrawTileIntoCanvas to ClearTileRectViaRHI_ELoad below.
	}

	// Pre-resolve + PIN every distinct icon up front so the per-tile draw loop
	// never co_awaits AsyncLoadObject mid-pass (SPEC 4.3, sync-texture-load-mid-pass).
	ResolveAndPinIcons();
}


void FCartographCompositor::Shutdown()
{
	// Signal the never-cancelled loop to fall out of its while() WITHOUT a
	// UE5Coro::Cancel(): the contract is "never cancelled". The loop checks
	// bRunning each tick and co_returns cleanly.
	bRunning = false;

	Store = nullptr;
	Grid = nullptr;
	ZBands = nullptr;
	ClassTable = nullptr;
	TileManager = nullptr;
	RenderTarget = nullptr;
	CurrentCanvas = nullptr;
	ScissorArea = { 0, 0, 0, 0 };

	PinnedIcons.Empty();
	ScratchHandles.Empty();
}


UCanvasRenderTarget2D* FCartographCompositor::GetRenderTarget() const
{
	return RenderTarget.Get();
}


bool FCartographCompositor::GetScissorForCanvas(const FCanvas* InCanvas, std::array<uint32, 4>& OutArea) const
{
	// Only scissor the canvas we are actively drawing a tile into. CurrentCanvas is
	// non-null only between BeginDraw and EndDraw inside RenderTile, and identifies
	// the FCanvas the hook should clip. A zero-area scissor (Max <= Min) means "no
	// active tile" - the hook should treat that as "do not scissor".
	if (InCanvas == nullptr || InCanvas != CurrentCanvas)
	{
		return false;
	}
	if (ScissorArea[2] <= ScissorArea[0] || ScissorArea[3] <= ScissorArea[1])
	{
		return false;
	}
	OutArea = ScissorArea;
	return true;
}


bool FCartographCompositor::IsReady() const
{
	return Store != nullptr
		&& Grid != nullptr
		&& ZBands != nullptr
		&& ClassTable != nullptr
		&& TileManager != nullptr
		&& RenderTarget.IsValid();
}


void FCartographCompositor::ResolveAndPinIcons()
{
	PinnedIcons.Reset();

	if (!ClassTable)
	{
		return;
	}

	TArray<TSoftObjectPtr<UTexture2D>> Icons;
	ClassTable->GatherIconAssets(Icons);

	PinnedIcons.Reserve(Icons.Num());
	for (const TSoftObjectPtr<UTexture2D>& Soft : Icons)
	{
		if (Soft.IsNull())
		{
			continue;
		}

		// Synchronous load is fine here: ResolveAndPinIcons runs ONCE at Initialize
		// and on explicit full-redraws, never inside the per-tile loop. LoadSynchronous
		// returns an already-resident texture cheaply on the 2nd call.
		UTexture2D* Texture = Soft.LoadSynchronous();
		if (Texture)
		{
			// TStrongObjectPtr roots the texture so it cannot be GC'd or streamed
			// out from under a mid-frame draw. This is the actual "pin".
			PinnedIcons.Emplace(Texture);
		}
	}

	CARTO_LOG("Compositor pinned %d icon textures", PinnedIcons.Num());
}


void FCartographCompositor::SetFullRedraw()
{
	if (!TileManager)
	{
		return;
	}

	// Re-pin in case the class table / icon set changed since Initialize, then
	// mark every tile dirty. We deliberately do NOT draw synchronously: the
	// never-cancelled loop drains the set over ceil(TILE_COUNT / K) frames so no
	// single submission approaches the TDR window (SPEC 4.2). This is the #10
	// full-redraw button.
	ResolveAndPinIcons();
	TileManager->MarkAllDirty();
}


void FCartographCompositor::SetZFilter(float MinZ, float MaxZ)
{
	if (MinZ > MaxZ)
	{
		Swap(MinZ, MaxZ);
	}

	if (MinZFilter == MinZ && MaxZFilter == MaxZ)
	{
		return;
	}

	MinZFilter = MinZ;
	MaxZFilter = MaxZ;

	// A Z-filter change can add/remove visible buildings in any tile. Re-converge
	// by marking everything dirty instead of an O(N) re-walk + full clear (the
	// legacy client-full-rebuild-on-zfilter cost). The loop redraws each tile
	// bounded; far tiles whose contents do not actually change still re-clear
	// their own ~1 MB rect, which is cheap vs the legacy 256 MB full clear.
	if (TileManager)
	{
		TileManager->MarkAllDirty();
	}
}


void FCartographCompositor::SetShowBuildings(bool bShow)
{
	if (bShowBuildings == bShow)
	{
		return;
	}

	bShowBuildings = bShow;

	// Toggling visibility re-converges the same way as a Z-filter change.
	if (TileManager)
	{
		TileManager->MarkAllDirty();
	}
}


// -----------------------------------------------------------------------------
// The never-cancelled convergent loop (SPEC 4.3).
// -----------------------------------------------------------------------------
UE5Coro::TCoroutine<> FCartographCompositor::TickConverge(UE5Coro::TLatentContext<> Context)
{
	using namespace UE5Coro;

	bRunning = true;

	// Capture the owning subsystem's world for the per-chunk
	// Begin/EndDrawCanvasToRenderTarget (EmitTileChunk). The atlas RenderTarget is a
	// content ASSET, so RT->GetWorld() is null and cannot serve as the canvas
	// WorldContextObject; Context.World is the live world the atlas draws into.
	WorldContext = Context.World;

	// Reused buffers kept in the coroutine frame so they survive suspensions.
	TArray<FTileId> PoppedTiles;
	TArray<FDrawable> Drawables;

	while (bRunning)
	{
		if (!(TileManager && TileManager->HasDirty()))
		{
			// Nothing to draw this tick; sleep one tick so the loop never spins the
			// game thread while the map is quiescent.
			co_await Latent::NextTick();
			continue;
		}

		const bool bRenderEnabled = CVarCartographRenderEnabled.GetValueOnGameThread();

		// THE GPU/TDR BOUND. EndDrawCanvasToRenderTarget only ENQUEUES an RDG pass, and
		// the RHI coalesces a frame's passes into ONE GPU submit - so a per-tile (or even
		// per-chunk) Begin/EndDraw is NOT a separate submit; the FRAME BOUNDARY is. We
		// therefore cap the building drawables emitted per FRAME and co_await a whole
		// frame (NextTick) once the cap is hit, so the render thread flushes and the GPU
		// drains before the next submit. Combined with the per-tile scissor (which clamps
		// every drawable's fill to one 256px tile), per-frame GPU fill stays
		// <= budget * tile_area - far under the Steam Deck's ~2-5s GPU TDR window even at
		// low clocks, no matter how many thousands of buildings pack a single tile. This
		// replaces the old per-tile FTickTimeBudget, which metered game-thread EMIT cycles
		// (not GPU execution) and so could not bound a dense tile's submit.
		int32 FrameBudget = FMath::Max(1, CVarCartographMaxDrawablesPerFrame.GetValueOnGameThread());

		// K still caps how many WHOLE tiles we START per drain pass; PopDirtyTiles orders
		// by AoI priority so the visible map converges first (SPEC 4.3 backpressure).
		const int32 K = FMath::Max(1, CVarCartographTilesPerFrame.GetValueOnGameThread());
		TileManager->PopDirtyTiles(K, PoppedTiles);

		for (const FTileId Tile : PoppedTiles)
		{
			if (!bRunning)
			{
				break;
			}

			// Gather this tile's sorted drawables ONCE (pure CPU; opens no canvas). A
			// LOCAL buffer (not a member) so a chunk slice can never be clobbered across
			// the NextTick suspensions below. Stays empty when rendering is disabled or
			// buildings are hidden -> the tile still gets exactly one clear-only chunk.
			Drawables.Reset();
			if (bRenderEnabled)
			{
				GatherTileDrawables(Tile, Drawables);
			}

			const int32 Num = Drawables.Num();
			int32 Offset = 0;
			bool bFirstChunk = true;

			// Emit the tile in <=FrameBudget-sized chunks. The do/while runs once for an
			// empty tile so a de-populated / hidden tile still clears (bFirstChunk emits
			// the clear quad). A dense tile spans several frames via the NextTick below.
			do
			{
				if (!bRunning)
				{
					break;
				}

				if (bRenderEnabled)
				{
					const int32 Take = FMath::Min(FrameBudget, Num - Offset);
					EmitTileChunk(Tile, Drawables, Offset, Take, bFirstChunk);
					Offset += Take;
					FrameBudget -= Take;
					bFirstChunk = false;
				}
				else
				{
					// Rendering disabled: consume the tile, touch no GPU.
					Offset = Num;
				}

				// Per-frame budget spent with this tile unfinished: yield a full frame
				// (forces the submit boundary), then refill for the next frame.
				if (FrameBudget <= 0 && Offset < Num)
				{
					co_await Latent::NextTick();
					FrameBudget = FMath::Max(1, CVarCartographMaxDrawablesPerFrame.GetValueOnGameThread());
				}
			}
			while (Offset < Num);

			// Budget spent exactly at a tile boundary: yield before the next tile so the
			// per-frame bound also holds ACROSS tiles (sparse tiles batch up to the cap).
			if (FrameBudget <= 0)
			{
				co_await Latent::NextTick();
				FrameBudget = FMath::Max(1, CVarCartographMaxDrawablesPerFrame.GetValueOnGameThread());
			}
		}

		// Always yield once per drain pass so the loop can never hot-spin even when every
		// popped tile was empty (all clear-only, zero budget consumed).
		co_await Latent::NextTick();
	}

	CARTO_LOG_DEBUG("Compositor TickConverge exited cleanly (Shutdown)");
	co_return;
}


// -----------------------------------------------------------------------------
// Publish the per-tile scissor the global FCanvas hook reads.
// -----------------------------------------------------------------------------
void FCartographCompositor::SetTileScissor(FTileId Tile)
{
	// {MinX, MinY, MaxX, MaxY} in atlas pixels. The global FCanvas::GetBatchedElements
	// hook (FCartographCanvasRenderItem, SPEC 4.2) reads this and clips every primitive
	// emitted into our canvas to the tile rect - which also clamps each drawable's GPU
	// FILL to <= one 256px tile no matter how oversized its footprint is.
	const FBox2f TileRect = FCartographTileManager::TileRect(Tile);
	ScissorArea = {
		(uint32)FMath::Max(0, FMath::FloorToInt(TileRect.Min.X)),
		(uint32)FMath::Max(0, FMath::FloorToInt(TileRect.Min.Y)),
		(uint32)FMath::Min(RENDER_TEXTURE_SIZE, FMath::CeilToInt(TileRect.Max.X)),
		(uint32)FMath::Min(RENDER_TEXTURE_SIZE, FMath::CeilToInt(TileRect.Max.Y))
	};
}


// -----------------------------------------------------------------------------
// Render exactly one tile, synchronously, in MaxDrawablesPerFrame chunks.
//
// SYNCHRONOUS entry (forced / test redraws only). It does NOT yield between chunks,
// so for a pathologically dense tile it can submit a lot in one frame; it is therefore
// kept OFF the hot path. The live drain path is TickConverge, which gathers + emits the
// same chunks but co_awaits a FRAME between them so every GPU submit is individually
// bounded (the actual Steam Deck TDR fix). Both share GatherTileDrawables/EmitTileChunk.
// -----------------------------------------------------------------------------
void FCartographCompositor::RenderTile(FTileId Tile)
{
	if (!IsReady())
	{
		return;
	}

	// Dedicated servers render nothing (SPEC 4.5). Guard defensively so a stray
	// RenderTile is a no-op rather than a crash.
	if (FPlatformProperties::IsServerOnly())
	{
		return;
	}

	// Master kill switch (r.Cartograph.RenderEnabled 0): issue no GPU work at all.
	if (!CVarCartographRenderEnabled.GetValueOnGameThread())
	{
		return;
	}

	TArray<FDrawable> Drawables;
	GatherTileDrawables(Tile, Drawables);

	const int32 Num = Drawables.Num();
	const int32 Max = FMath::Max(1, CVarCartographMaxDrawablesPerFrame.GetValueOnGameThread());
	int32 Offset = 0;
	bool bFirstChunk = true;

	// do/while runs once for an empty tile so it still clears (bFirstChunk).
	do
	{
		const int32 Take = FMath::Min(Max, Num - Offset);
		EmitTileChunk(Tile, Drawables, Offset, Take, bFirstChunk);
		Offset += Take;
		bFirstChunk = false;
	}
	while (Offset < Num);
}


// -----------------------------------------------------------------------------
// Gather one tile's sorted drawables (pure CPU; opens no canvas, touches no GPU).
// -----------------------------------------------------------------------------
void FCartographCompositor::GatherTileDrawables(FTileId Tile, TArray<FDrawable>& Out)
{
	Out.Reset();

	// Buildings hidden: gather nothing. The caller still emits one clear-only chunk,
	// the correct converged state for "hide buildings".
	if (!bShowBuildings)
	{
		return;
	}

	const FBox2f TileRect = FCartographTileManager::TileRect(Tile);

	// Collect the buildings whose footprint touches this tile (spatial grid), keep only
	// those inside the active Z range, de-dup multi-cell hits, then sort BACK-TO-FRONT
	// by Z so overlapping translucent quads composite identically to the legacy Z-sorted
	// draw (SPEC 4.1/4.2, painters' order; Q12 pixel-identity).
	ScratchHandles.Reset();

	// World-space query box for this tile rect (grid is keyed in world cm).
	const FVector2D WorldMin = CartographCoords::ScreenToWorld(TileRect.Min);
	const FVector2D WorldMax = CartographCoords::ScreenToWorld(TileRect.Max);
	const FBox2f WorldQueryBox(
		FVector2f((float)FMath::Min(WorldMin.X, WorldMax.X), (float)FMath::Min(WorldMin.Y, WorldMax.Y)),
		FVector2f((float)FMath::Max(WorldMin.X, WorldMax.X), (float)FMath::Max(WorldMin.Y, WorldMax.Y)));

	Grid->QueryRect(WorldQueryBox, ScratchHandles);

	if (ScratchHandles.Num() == 0)
	{
		return;
	}

	// The grid returns duplicates for multi-cell buildings and false positives
	// (cell-granular). De-dup so a building straddling several cells of this tile is
	// not drawn multiple times (translucent over-draw + wasted work).
	TSet<FBuildingHandle> Seen;
	Seen.Reserve(ScratchHandles.Num());

	// Exact Z-filter range in world cm: honor the precise slider range against each
	// record's Z, not just band granularity.
	const float MinZ = MinZFilter;
	const float MaxZ = MaxZFilter;

	Out.Reserve(ScratchHandles.Num());

	for (const FBuildingHandle Handle : ScratchHandles)
	{
		bool bAlreadySeen = false;
		Seen.Add(Handle, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}

		const FBuildingRecord* Record = Store->Find(Handle);
		if (!Record)
		{
			// Stale handle (slot freed/recycled since the grid insert): skip. The grid
			// is reconciled by the owning subsystem; the generation guard made Find
			// return null so this is harmless.
			continue;
		}

		const float WorldZ = Record->Pos.Z;
		if (WorldZ < MinZ || WorldZ > MaxZ)
		{
			continue;
		}

		const FClassDrawInfo* Info = ClassTable->FindInfo(Record->ClassId);
		if (!Info)
		{
			continue;
		}

		// Recompute draw geometry from the slim record + class info + side-tables.
		// PURE: no engine queries, no stored cache (SPEC 4.1). Pixel-identical to the
		// legacy FillInCache (Q12). Output is already in atlas pixel space.
		FDrawable Drawable;
		Drawable.Z = WorldZ;
		if (!FCartographClassDrawTable::ComputeDrawGeometry(*Record, *Info, *Store, Drawable.Geometry))
		{
			// Not drawable (zero footprint / zero stroke) - matches legacy early-out.
			continue;
		}

		Out.Emplace(MoveTemp(Drawable));
	}

	// Back-to-front: low Z first, exactly the order the legacy Z-sorted array drew.
	// Stable so same-Z buildings keep a deterministic (grid-query) order. Because the
	// chunked emit slices this sorted array into CONTIGUOUS ranges and draws them in
	// order, cross-chunk compositing is identical to a single-pass draw (Q12 preserved).
	Algo::StableSortBy(Out, &FDrawable::Z);
}


// -----------------------------------------------------------------------------
// Render ONE chunk of a tile: a single Begin/EndDrawCanvasToRenderTarget pass.
// -----------------------------------------------------------------------------
void FCartographCompositor::EmitTileChunk(FTileId Tile, const TArray<FDrawable>& Drawables, int32 Offset, int32 Count, bool bEmitClear)
{
	UCanvasRenderTarget2D* RT = RenderTarget.Get();
	if (!RT)
	{
		return;
	}

	// The WorldContextObject for Begin/EndDrawCanvasToRenderTarget. The atlas RT is a
	// content ASSET, so RT->GetWorld() is null; use the world TickConverge captured.
	UWorld* World = WorldContext.Get();
	if (!World)
	{
		CARTO_LOG_ERROR("EmitTileChunk: no world context (TickConverge must set WorldContext before drawing)");
		return;
	}

	// Publish the scissor BEFORE opening the canvas so the global hook clips every batch
	// to the tile rect (and clamps per-drawable GPU fill to <= one tile).
	SetTileScissor(Tile);

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, Canvas, CanvasSize, Context);
	if (!Canvas || !Canvas->Canvas)
	{
		CARTO_LOG_ERROR("EmitTileChunk: BeginDrawCanvasToRenderTarget produced a null canvas");
		ScissorArea = { 0, 0, 0, 0 };
		return;
	}

	// CurrentCanvas lets the scissor hook recognise OUR canvas (it scissors only when
	// Canvas == CurrentCanvas).
	CurrentCanvas = Canvas->Canvas;

	// (1) Tile-bounded opaque clear - ONLY on the tile's FIRST chunk. Subsequent chunks
	// of the same tile must NOT clear: they composite onto the prior chunks' pixels,
	// which the persistent atlas preserves across successive Begin/EndDraw (the SAME
	// content-preserve the per-tile drain already relies on for every OTHER tile, so no
	// new assumption). ~1 MB 256px quad vs the legacy full-screen 67 M-texel clear.
	if (bEmitClear)
	{
		const FBox2f TileRect = FCartographTileManager::TileRect(Tile);
		FCanvasTileItem ClearItem(
			FVector2D(TileRect.Min.X, TileRect.Min.Y),
			FVector2D(TileRect.Max.X - TileRect.Min.X, TileRect.Max.Y - TileRect.Min.Y),
			FLinearColor(0.f, 0.f, 0.f, 0.f));  // clear to transparent (matches legacy {0,0,0,0})
		ClearItem.BlendMode = SE_BLEND_Opaque;
		Canvas->DrawItem(ClearItem);
	}

	// (2) Emit this chunk's slice of the (Z-sorted) drawables. Count==0 is valid: an
	// empty / hidden tile is just the clear above.
	const int32 StartIdx = FMath::Max(0, Offset);
	const int32 EndIdx = FMath::Min(StartIdx + FMath::Max(0, Count), Drawables.Num());
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		DrawGeometry(Canvas, Drawables[i].Geometry);
	}

	// EndDraw enqueues this chunk. The caller co_awaits a FRAME between chunks so the RHI
	// closes the command buffer and the GPU drains before the next submit (the bound).
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);

	CurrentCanvas = nullptr;
	ScissorArea = { 0, 0, 0, 0 };
}


// -----------------------------------------------------------------------------
// Emit FCanvas items for one computed geometry. Ports the legacy per-primitive
// draw (RedrawMapCoroutine switch) but consumes pixel-space FDrawGeometry, so it
// does NO cm->px conversion and NO awaits (the await lives one level up, per
// tile). Pixel-identical output to the legacy path (SPEC Q12).
// -----------------------------------------------------------------------------
void FCartographCompositor::DrawGeometry(UCanvas* Canvas, const FDrawGeometry& Geometry)
{
	switch (Geometry.Type)
	{
	case EBuildingDrawType::Invalid:
		break;

	case EBuildingDrawType::Icon:
	{
		// Resolve the pinned icon. The texture was pre-pinned by ResolveAndPinIcons,
		// so Get() returns a resident texture with no AsyncLoad (SPEC 4.3). If a
		// late-registered class slipped past pinning we skip rather than block.
		const UTexture2D* Texture = Geometry.Icon.Get();
		if (!Texture || !Texture->GetResource())
		{
			CARTO_LOG_VERBOSE("Icon texture not resident at draw; skipping (was it pinned?)");
			break;
		}

		// FDrawGeometry.ScreenPos/Size are already in atlas pixels. Legacy drew the
		// tile centred on ScreenPos with PivotPoint {0.5,0.5} and Rotation in
		// degrees; reproduce exactly.
		FCanvasTileItem TileItem(
			FVector2D(Geometry.ScreenPos.X, Geometry.ScreenPos.Y),
			Texture->GetResource(),
			FVector2D(Geometry.Size.X, Geometry.Size.Y),
			FVector2D(0.f, 0.f),
			FVector2D(1.f, 1.f),
			FLinearColor::White);
		TileItem.PivotPoint = FVector2D(0.5, 0.5);
		TileItem.BlendMode = FCanvas::BlendToSimpleElementBlend(EBlendMode::BLEND_Translucent);
		TileItem.Rotation = FRotator(0.f, Geometry.RotationDeg, 0.f);  // yaw-only top-down

		Canvas->DrawItem(TileItem);
		break;
	}

	case EBuildingDrawType::Rectangle:
	{
		// Filled footprint quad (legacy MainColor fill).
		FCanvasTileItem TileItem(
			FVector2D(Geometry.ScreenPos.X, Geometry.ScreenPos.Y),
			FVector2D(Geometry.Size.X, Geometry.Size.Y),
			Geometry.MainColor);
		TileItem.PivotPoint = FVector2D(0.5, 0.5);
		TileItem.BlendMode = FCanvas::BlendToSimpleElementBlend(EBlendMode::BLEND_Translucent);
		TileItem.Rotation = FRotator(0.f, Geometry.RotationDeg, 0.f);

		Canvas->DrawItem(TileItem);

		// Outline: four lines around the screen-space corners (CW from top-left).
		if (Geometry.OutlineThickness > 0.f)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				const FVector2f A = Geometry.Corners[i];
				const FVector2f B = Geometry.Corners[(i + 1) % 4];
				FCanvasLineItem LineItem(FVector2D(A.X, A.Y), FVector2D(B.X, B.Y));
				LineItem.LineThickness = Geometry.OutlineThickness;
				LineItem.SetColor(Geometry.OutlineColor);
				Canvas->DrawItem(LineItem);
			}
		}
		break;
	}

	case EBuildingDrawType::Spline:
	{
		const TArray<FVector2f>& Points = Geometry.SplinePointsScreen;
		const int32 Num = Points.Num();
		for (int32 j = 0; j < Num - 1; ++j)
		{
			FCanvasLineItem LineItem(
				FVector2D(Points[j].X, Points[j].Y),
				FVector2D(Points[j + 1].X, Points[j + 1].Y));
			LineItem.LineThickness = Geometry.StrokeThickness;
			LineItem.SetColor(Geometry.StrokeColor);
			Canvas->DrawItem(LineItem);
		}
		break;
	}

	case EBuildingDrawType::Wire:
	case EBuildingDrawType::Beam:
	{
		// Both are a single segment: ScreenPos (start) -> WireOrBeamEndScreen.
		FCanvasLineItem LineItem(
			FVector2D(Geometry.ScreenPos.X, Geometry.ScreenPos.Y),
			FVector2D(Geometry.WireOrBeamEndScreen.X, Geometry.WireOrBeamEndScreen.Y));
		LineItem.LineThickness = Geometry.StrokeThickness;
		LineItem.SetColor(Geometry.StrokeColor);
		Canvas->DrawItem(LineItem);
		break;
	}

	default:
		break;
	}
}


// -----------------------------------------------------------------------------
// COMMITTED Phase-A fallback: scissored, content-preserving (ELoad) tile clear
// on the render thread (SPEC Q3 / §4.2). Only used IF the persistent-RT preserve
// acceptance test fails on the fork; otherwise the FCanvas tile-clear quad in
// EmitTileChunk (first chunk) already does the bounded clear and this path is unused.
// -----------------------------------------------------------------------------
void FCartographCompositor::ClearTileRectViaRHI_ELoad(FTileId Tile)
{
	UCanvasRenderTarget2D* RT = RenderTarget.Get();
	if (!RT)
	{
		return;
	}

	FTextureRenderTargetResource* Resource = RT->GetRenderTargetResource();
	if (!Resource)
	{
		return;
	}

	const FBox2f TileRect = FCartographTileManager::TileRect(Tile);
	const FIntRect IntRect(
		FMath::Max(0, FMath::FloorToInt(TileRect.Min.X)),
		FMath::Max(0, FMath::FloorToInt(TileRect.Min.Y)),
		FMath::Min(RENDER_TEXTURE_SIZE, FMath::CeilToInt(TileRect.Max.X)),
		FMath::Min(RENDER_TEXTURE_SIZE, FMath::CeilToInt(TileRect.Max.Y)));

	// Clear to transparent black (matches the FCanvas clear-quad colour).
	const FLinearColor ClearColor(0.f, 0.f, 0.f, 0.f);

	ENQUEUE_RENDER_COMMAND(CartographClearTileELoad)(
		[Resource, IntRect, ClearColor](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* RHITexture = Resource->GetRenderTargetTexture();
			if (!RHITexture)
			{
				return;
			}

			// SPIKE(Q3): ERenderTargetLoadAction::ELoad must PRESERVE the rest of
			// the atlas while we overwrite only the scissored tile rect. This is the
			// documented preserve-failure fallback; the exact FRHIRenderPassInfo /
			// ClearRenderTargetView signature must be verified against the CSS RHI
			// (no Engine/ source on disk to confirm the exported overload).
			FRHIRenderPassInfo RPInfo(RHITexture, ERenderTargetActions::Load_Store);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("CartographClearTileELoad"));
			{
				// Scissor bounds the clear to exactly this tile - everything else is
				// preserved by the Load action above.
				RHICmdList.SetScissorRect(true, IntRect.Min.X, IntRect.Min.Y, IntRect.Max.X, IntRect.Max.Y);

				// A scissored DrawClearQuad clears only inside the scissor rect.
				DrawClearQuad(RHICmdList, ClearColor);

				RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
			}
			RHICmdList.EndRenderPass();
		});
}

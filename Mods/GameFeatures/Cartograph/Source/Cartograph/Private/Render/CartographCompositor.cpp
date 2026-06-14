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


namespace
{
	/** Default per-frame budget if the world / frame delta cannot be read.
	 *  16.6 ms ~= one 60 fps frame; the fraction is applied on top. */
	constexpr double DefaultFrameSeconds = 1.0 / 60.0;

	/** Read the current frame's delta seconds for the frame-relative budget. */
	double GetFrameSeconds(const UWorld* World)
	{
		if (World)
		{
			const float Delta = World->GetDeltaSeconds();
			if (Delta > KINDA_SMALL_NUMBER)
			{
				return (double)Delta;
			}
		}
		return DefaultFrameSeconds;
	}
}


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

	// World for frame timing. The latent action itself is hosted in Context.World
	// (the owning subsystem's world); prefer the render target's world if present so
	// the per-frame delta is read from the exact UWorld the atlas draws into, which
	// is identical to the legacy path. Both resolve to the same world in practice.
	const UWorld* World = Context.World;
	if (const UCanvasRenderTarget2D* RT = RenderTarget.Get())
	{
		if (UWorld* RTWorld = RT->GetWorld())
		{
			World = RTWorld;
		}
	}

	// Reused across ticks - the contract says to keep the budget OUTSIDE the loop.
	// FTickTimeBudget tracks cycles spent this tick; co_awaiting it yields to the
	// next tick once the budget is exhausted, otherwise keeps running.
	// We rebuild it per tick because the budget is FRAME-RELATIVE (a fraction of
	// THIS frame's delta), not a fixed ms (SPEC 4.3, issue #10): the frame delta
	// changes, so the per-tick budget must track it.

	TArray<FTileId> PoppedTiles;

	while (bRunning)
	{
		// Frame-relative budget for THIS drain batch: FrameBudgetFraction (default
		// 0.15) of the current frame time - a FRACTION of frame time, NOT an absolute
		// ms (SPEC 4.3, the correct answer to issue #10). Constructed per while-batch
		// so it tracks the current frame's delta; FTickTimeBudget::await_resume
		// auto-resets its clock when the coroutine actually suspends and resumes a
		// tick later, so awaiting this same object repeatedly across ticks within a
		// batch is the library's intended usage (hence it lives outside the inner
		// for, in the coroutine frame, surviving suspensions).
		const double FrameSeconds = GetFrameSeconds(World);
		const float BudgetFraction = FMath::Clamp(
			CVarCartographFrameBudgetFraction.GetValueOnGameThread(), 0.01f, 1.0f);
		Latent::FTickTimeBudget Budget =
			Latent::FTickTimeBudget::Seconds(FrameSeconds * (double)BudgetFraction);

		// K = hard cap on tiles per tick so a single frame can never attempt an
		// unbounded drain even if the soft time budget is generous.
		const int32 K = FMath::Max(1, CVarCartographTilesPerFrame.GetValueOnGameThread());

		if (TileManager && TileManager->HasDirty())
		{
			// PopDirtyTiles clears the popped bits and orders by AoI priority so the
			// VISIBLE map converges first (SPEC 4.3 backpressure). Tiles re-dirtied
			// mid-pass simply re-set their bit for a later tick (coalesced).
			TileManager->PopDirtyTiles(K, PoppedTiles);

			for (const FTileId Tile : PoppedTiles)
			{
				if (!bRunning)
				{
					break;
				}

				// One tile = one Begin/EndDraw = one separate GPU command buffer.
				// This is the structural TDR fix: the submission for this tile is
				// bounded to O(buildings in the tile), never the whole factory.
				RenderTile(Tile);

				// Await the budget ONCE PER TILE (not per primitive). This deletes
				// the legacy ~9*N per-primitive awaiter + FPlatformTime::Cycles()
				// reads (per-primitive-budget-suspension-overhead). If the budget is
				// already spent this resumes next tick; otherwise it keeps draining.
				co_await Budget;
			}
		}
		else
		{
			// Nothing to draw this tick. Sleep one tick so the loop does not spin
			// the game thread when the map is quiescent.
			co_await Latent::NextTick();
		}
	}

	CARTO_LOG_DEBUG("Compositor TickConverge exited cleanly (Shutdown)");
	co_return;
}


// -----------------------------------------------------------------------------
// Render exactly one tile (synchronous, separate command buffer).
// -----------------------------------------------------------------------------
void FCartographCompositor::RenderTile(FTileId Tile)
{
	if (!IsReady())
	{
		return;
	}

	// Dedicated servers render nothing (SPEC 4.5). The owning subsystem normally
	// never starts the compositor on a dedicated server, but guard defensively so
	// a stray RenderTile is a no-op rather than a crash.
	if (FPlatformProperties::IsServerOnly())
	{
		return;
	}

	UCanvasRenderTarget2D* RT = RenderTarget.Get();
	if (!RT)
	{
		return;
	}

	// SPIKE(Q3): the WorldContextObject for Begin/EndDrawCanvasToRenderTarget.
	// The legacy module passed `this` (a UObject). The compositor is a plain C++
	// object, so we resolve the world from the render target. This requires the
	// owning subsystem to have created the RT with a valid world context (e.g.
	// UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(WorldContext, ...)) so
	// UCanvasRenderTarget2D::GetWorld() is non-null; verify on the fork.
	UWorld* World = RT->GetWorld();
	if (!World)
	{
		CARTO_LOG_ERROR("RenderTile: render target has no world (RT must be created with a world context)");
		return;
	}

	// Tile pixel rect (ragged edge tiles already clamped to RENDER_TEXTURE_SIZE).
	const FBox2f TileRect = FCartographTileManager::TileRect(Tile);

	// Publish the scissor BEFORE opening the canvas so the global
	// FCanvas::GetBatchedElements hook (reused from the legacy
	// FCartographCanvasRenderItem mechanism, SPEC 4.2) clips every batch emitted
	// for THIS tile to the tile rect. {MinX, MinY, MaxX, MaxY} in atlas pixels.
	ScissorArea = {
		(uint32)FMath::Max(0, FMath::FloorToInt(TileRect.Min.X)),
		(uint32)FMath::Max(0, FMath::FloorToInt(TileRect.Min.Y)),
		(uint32)FMath::Min(RENDER_TEXTURE_SIZE, FMath::CeilToInt(TileRect.Max.X)),
		(uint32)FMath::Min(RENDER_TEXTURE_SIZE, FMath::CeilToInt(TileRect.Max.Y))
	};

	// Open the canvas for THIS tile only. Each Begin/EndDraw pair is its own GPU
	// command-buffer submission - the guaranteed TDR bound (SPEC 4.2). We do NOT
	// batch multiple tiles into one Begin/EndDraw: that would re-merge the
	// submissions and reopen the TDR window.
	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize;
	FDrawToRenderTargetContext Context;
	// SPIKE(Q3): UKismetRenderingLibrary::Begin/EndDrawCanvasToRenderTarget signature
	// (WorldContextObject, RT, out UCanvas*, out FVector2D Size, out FDrawToRenderTargetContext)
	// matches the legacy module's usage on this fork; confirm the exported overload on
	// CSS (no Engine/ source on disk). A UWorld* is a valid WorldContextObject.
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, Canvas, CanvasSize, Context);
	if (!Canvas || !Canvas->Canvas)
	{
		CARTO_LOG_ERROR("RenderTile: BeginDrawCanvasToRenderTarget produced a null canvas");
		ScissorArea = { 0, 0, 0, 0 };
		return;
	}

	// CurrentCanvas lets the scissor hook recognise OUR canvas (it scissors only
	// when Canvas == CurrentCanvas), exactly as the legacy code keyed off
	// UCartographGameInstanceModule::Instance->CurrentCanvas.
	CurrentCanvas = Canvas->Canvas;

	DrawTileIntoCanvas(Tile, Canvas);

	// EndDraw flushes THIS tile as a separate command buffer. Because the atlas is
	// never fully cleared (only this tile's rect was cleared above) and
	// bAutoGenerateMips=false, no full-texture pass is kicked here.
	// SPIKE(Q3): if the fork implicitly re-clears the whole target on the NEXT
	// BeginDraw (preserve-failure), the committed fallback is ClearTileRectViaRHI_ELoad
	// (ELoad load action + RHI scissor), reachable today via the RHI dep.
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);

	CurrentCanvas = nullptr;
	ScissorArea = { 0, 0, 0, 0 };
}


// -----------------------------------------------------------------------------
// Draw one tile into an already-open canvas.
// -----------------------------------------------------------------------------
void FCartographCompositor::DrawTileIntoCanvas(FTileId Tile, UCanvas* Canvas)
{
	const FBox2f TileRect = FCartographTileManager::TileRect(Tile);

	// (1) Scissored hardware clear bounded to the tile rect. A tile-sized opaque
	// quad overwrites EXACTLY this tile - ~1 MB of texels at 256 px vs the legacy
	// full-screen 67 M-texel opaque clear every redraw (fullscreen-clear-every-redraw).
	// The scissor (set in RenderTile) additionally guarantees no spill beyond the
	// rect even if the quad rounds outward.
	{
		FCanvasTileItem ClearItem(
			FVector2D(TileRect.Min.X, TileRect.Min.Y),
			FVector2D(TileRect.Max.X - TileRect.Min.X, TileRect.Max.Y - TileRect.Min.Y),
			FLinearColor(0.f, 0.f, 0.f, 0.f));  // clear to transparent (matches legacy {0,0,0,0})
		ClearItem.BlendMode = SE_BLEND_Opaque;
		Canvas->DrawItem(ClearItem);
	}

	if (!bShowBuildings)
	{
		// Cleared but nothing drawn: the tile is now empty, which is the correct
		// converged state for "hide buildings".
		return;
	}

	// (2) Collect the buildings whose footprint touches this tile (spatial grid),
	// keep only those inside the active Z range, de-dup multi-cell hits, then draw
	// BACK-TO-FRONT by Z so overlapping translucent quads composite identically to
	// the legacy Z-sorted draw (SPEC 4.1/4.2, painters' order; Q12 pixel-identity).
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
	// (cell-granular). De-dup so a building straddling several cells of this tile
	// is not drawn multiple times (translucent over-draw + wasted work).
	TSet<FBuildingHandle> Seen;
	Seen.Reserve(ScratchHandles.Num());

	// Exact Z-filter range in world cm: honor the precise slider range against each
	// record's Z, not just band granularity.
	const float MinZ = MinZFilter;
	const float MaxZ = MaxZFilter;

	// Drawables collected with their Z so we can sort back-to-front before emitting.
	struct FDrawable
	{
		float Z;
		FDrawGeometry Geometry;
	};
	TArray<FDrawable> Drawables;
	Drawables.Reserve(ScratchHandles.Num());

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
			// Stale handle (slot freed/recycled since the grid insert): skip. The
			// grid is reconciled by the owning subsystem; the generation guard made
			// Find return null so this is harmless.
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
		// PURE: no engine queries, no stored cache (SPEC 4.1). Pixel-identical to
		// the legacy FillInCache (Q12). Output is already in atlas pixel space.
		FDrawable Drawable;
		Drawable.Z = WorldZ;
		if (!FCartographClassDrawTable::ComputeDrawGeometry(*Record, *Info, *Store, Drawable.Geometry))
		{
			// Not drawable (zero footprint / zero stroke) - matches legacy early-out.
			continue;
		}

		Drawables.Emplace(MoveTemp(Drawable));
	}

	// Back-to-front: low Z first, exactly the order the legacy Z-sorted array drew.
	// Stable so same-Z buildings keep a deterministic (grid-query) order.
	Algo::StableSortBy(Drawables, &FDrawable::Z);

	for (const FDrawable& Drawable : Drawables)
	{
		DrawGeometry(Canvas, Drawable.Geometry);
	}

	// NOTE on the "await per K buildings within a large tile" half of the contract
	// (SPEC 4.3): the budget await lives one level up in TickConverge (once per
	// tile). RenderTile is by contract SYNCHRONOUS / no budget, and a 256 px tile's
	// building count is naturally bounded, so the per-tile EndDraw submission is the
	// unit that guarantees the TDR bound. If profiling shows a single tile's
	// game-thread EMIT time (not GPU time) exceeds the frame budget, the refinement
	// is to split a dense tile's handle list into fixed-size chunks
	// across multiple Begin/EndDraw passes with ELoad in TickConverge - the data
	// path (grid query + ComputeDrawGeometry) is already chunk-friendly. Left as a
	// documented follow-up rather than speculative complexity.
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
// DrawTileIntoCanvas already does the bounded clear and this path is unused.
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

#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"
#include "UE5Coro/UE5Coro.h"

#include <array>

// =============================================================================
// CartographCompositor.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "compositor" agent (CartographCompositor.cpp).
// -----------------------------------------------------------------------------
// The never-cancelled convergent compositor (SPEC 4.2 Phase A, 4.3).
//
// Owns the persistent UCanvasRenderTarget2D (the 64 MB atlas), never fully
// cleared, bAutoGenerateMips=false. Every frame it drains the dirty-tile set
// under a frame-relative budget (UE5Coro FTickTimeBudget, fraction =
// r.Cartograph.FrameBudgetFraction): pops up to K tiles, and for each issues a
// scissored RHIClearRenderTargetView bounded to the tile rect + redraws only
// the buildings the grid reports in that tile (Z-band filtered), then
// EndDrawCanvasToRenderTarget. Each tile is a SEPARATE GPU command-buffer
// submission - the guaranteed TDR fix. The compositor is NEVER cancelled and
// the dirty set only shrinks during a pass; await is per-tile (or per K
// buildings), NEVER per primitive (SPEC 4.3).
//
// This is the Phase-A/default backend (ECartographRenderBackend::TiledCanvas).
// The Slate and instance backends are separate classes selected by CVar.
//
// Dependencies (injected by the owning subsystem at construction; the
// compositor does not own them): the building store, spatial grid, Z-band
// index, class draw table, and tile manager. The exact injection mechanism
// (constructor args / Init) is the agent's choice; the FROZEN surface is the
// lifecycle + drain entry points below.
// =============================================================================
class FCartographBuildingStore;
class FCartographSpatialGrid;
class FCartographZBandIndex;
class FCartographClassDrawTable;
class FCartographTileManager;
class UCanvasRenderTarget2D;
class UCanvas;
class FCanvas;
class UTexture2D;
class UWorld;
struct FDrawToRenderTargetContext;


class CARTOGRAPH_API FCartographCompositor
{
public:
	FCartographCompositor() = default;
	~FCartographCompositor() = default;

	FCartographCompositor(const FCartographCompositor&) = delete;
	FCartographCompositor& operator=(const FCartographCompositor&) = delete;

	/**
	 * Bind the data spine + the persistent render target. Call once after the
	 * subsystems exist and the atlas is created. The compositor keeps raw
	 * non-owning pointers; the owning subsystem outlives the compositor.
	 */
	void Initialize(
		FCartographBuildingStore* InStore,
		FCartographSpatialGrid* InGrid,
		FCartographZBandIndex* InZBands,
		FCartographClassDrawTable* InClassTable,
		FCartographTileManager* InTileManager,
		UCanvasRenderTarget2D* InRenderTarget);

	/** Release references; does not destroy the injected subsystems. */
	void Shutdown();

	/** The persistent atlas the map UI samples. Valid after Initialize. */
	UCanvasRenderTarget2D* GetRenderTarget() const;

	// -------------------------------------------------------------------------
	// Convergence. The compositor runs forever; it is started ONCE and never
	// cancel-restarted (SPEC 4.3, kills the legacy cancel/restart thrash).
	// -------------------------------------------------------------------------

	/**
	 * One never-cancelled latent loop: each frame, drain up to K dirty tiles
	 * (K = r.Cartograph.TilesPerFrame) under FTickTimeBudget(BudgetFraction =
	 * r.Cartograph.FrameBudgetFraction), prioritizing the current AoI. Awaits
	 * the budget ONCE per tile (or per K buildings within a large tile), never
	 * per primitive. The coroutine completes only on Shutdown.
	 *
	 * Started once by the owning subsystem, which passes itself (a UObject) as the
	 * latent context: the compositor is a plain C++ object, so it cannot be its own
	 * UE5Coro world context. TLatentContext supplies the latent-action Target + the
	 * containing UWorld explicitly (implicitly from the owner `this`), bypassing the
	 * default first-parameter/`this` world detection.
	 */
	UE5Coro::TCoroutine<> TickConverge(UE5Coro::TLatentContext<> Context);

	/**
	 * Render exactly one tile NOW (synchronous, no budget). For the compositor's
	 * own drain loop and for forced/test redraws. Issues a scissored hardware
	 * clear bounded to the tile rect + redraws the grid+Z-band-filtered
	 * buildings + EndDraw (a separate GPU command buffer). Honors the current
	 * Z-filter range. Big-O: O(buildings in the tile).
	 */
	void RenderTile(FTileId Tile);

	/**
	 * Mark every populated tile dirty (the #10 full-redraw button). Does NOT
	 * draw synchronously - the never-cancelled loop drains it over
	 * ceil(dirtyTiles/K) frames so no submission approaches the TDR window.
	 */
	void SetFullRedraw();

	// -------------------------------------------------------------------------
	// Filters that affect what RenderTile draws. Setting these marks the
	// affected tiles dirty so the change converges without a full clear.
	// -------------------------------------------------------------------------

	/** Set the Z height-filter range (world cm) used by tile redraws. Replaces
	 *  the legacy O(N) re-walk + 256 MB clear with a dirty-tile re-converge. */
	void SetZFilter(float MinZ, float MaxZ);

	/** Toggle whether buildings draw at all (legacy DoShowBuildings). */
	void SetShowBuildings(bool bShow);

	// -------------------------------------------------------------------------
	// Scissor integration (SPEC 4.2: reuse the FCartographCanvasRenderItem
	// ScissorArea mechanism). NOT part of the originally-frozen surface - added
	// so the global FCanvas::GetBatchedElements hook can read the compositor's
	// active per-tile scissor the same way the legacy item read
	// UCartographGameInstanceModule::Instance->{CurrentCanvas,ScissorArea}.
	// Reported in contractDeviations. See the .cpp for the required wiring.
	// -------------------------------------------------------------------------

	/**
	 * If InCanvas is the FCanvas this compositor is currently drawing a tile into,
	 * write the active tile scissor {MinX, MinY, MaxX, MaxY} (atlas px) to OutArea
	 * and return true; otherwise return false (the hook must NOT scissor a canvas
	 * that is not ours). O(1).
	 */
	bool GetScissorForCanvas(const FCanvas* InCanvas, std::array<uint32, 4>& OutArea) const;

private:
	// Implementation (owning agent): a persistent FTextureRenderTargetResource /
	// FCanvas pair, the scissored clear via UKismetRenderingLibrary or the
	// RHI/RDG ELoad fallback (SPEC Q3), the AoI-prioritized drain, and the
	// pre-pinned icon set. The Z-filter range + show flag are member state.

	// -------------------------------------------------------------------------
	// Injected, non-owning data spine + render target (set in Initialize).
	// The owning subsystem outlives the compositor and owns the lifetime of all
	// of these; the compositor keeps only raw pointers.
	// -------------------------------------------------------------------------
	FCartographBuildingStore* Store = nullptr;
	FCartographSpatialGrid* Grid = nullptr;
	// ZBands is injected per the contract. The per-tile draw currently filters by an
	// exact per-record Z test (O(hits), precise to the slider, see DrawTileIntoCanvas)
	// rather than intersecting the band lists; ZBands is retained as the fast coarse
	// pre-filter / painters'-order source for a future optimization and to keep the
	// injected surface intact.
	FCartographZBandIndex* ZBands = nullptr;
	FCartographClassDrawTable* ClassTable = nullptr;
	FCartographTileManager* TileManager = nullptr;

	/** GC is the owning subsystem's responsibility; we hold a weak ref so a GC
	 *  of the atlas during a stall can never dangle a draw. */
	TWeakObjectPtr<UCanvasRenderTarget2D> RenderTarget;

	/** WorldContextObject for Begin/EndDrawCanvasToRenderTarget. Captured from the
	 *  TickConverge latent context (the owning subsystem's world). The atlas
	 *  RenderTarget is a content ASSET, so RT->GetWorld() is null and cannot serve
	 *  as the context (the legacy module passed the owning UObject instead); we
	 *  capture the live world here. RenderTile only runs inside TickConverge. */
	TWeakObjectPtr<UWorld> WorldContext;

	// -------------------------------------------------------------------------
	// Z-filter range + show flag (member state honored by every RenderTile).
	// -------------------------------------------------------------------------
	float MinZFilter = -TNumericLimits<float>::Max();
	float MaxZFilter = TNumericLimits<float>::Max();
	bool bShowBuildings = true;

	// -------------------------------------------------------------------------
	// Scissor mechanism state (SPEC 4.2: reuse the CartographCanvasRenderItem
	// ScissorArea path). Public-by-friend so the global FCanvas::GetBatchedElements
	// hook can read it the same way the legacy FCartographCanvasRenderItem reads
	// UCartographGameInstanceModule::Instance->ScissorArea. The owning subsystem
	// repoints that hook at the compositor (integration step; see .cpp notes).
	//
	// {MinX, MinY, MaxX, MaxY} in atlas pixel space for the tile currently being
	// drawn. CurrentCanvas identifies our FCanvas so the hook only scissors ours.
	// -------------------------------------------------------------------------
	std::array<uint32, 4> ScissorArea = { 0, 0, 0, 0 };
	FCanvas* CurrentCanvas = nullptr;

	/** True only while a TickConverge loop is live; flipped false by Shutdown so
	 *  the never-cancelled loop exits cleanly without a UE5Coro::Cancel(). */
	bool bRunning = false;

	/** Distinct icon textures pinned for the lifetime of the compositor so a tile
	 *  redraw never AsyncLoads mid-pass (SPEC 4.3, fixes sync-texture-load-mid-pass).
	 *  TStrongObjectPtr roots each texture against GC without reflection (this is a
	 *  plain C++ class, not a UObject, so no UPROPERTY here). Rebuilt by
	 *  ResolveAndPinIcons. The render target itself is rooted by the owning
	 *  subsystem; we keep only a weak ref to it. */
	TArray<TStrongObjectPtr<UTexture2D>> PinnedIcons;

	/** Scratch reused across RenderTile calls to avoid per-tile allocation. */
	TArray<FBuildingHandle> ScratchHandles;

	// -------------------------------------------------------------------------
	// Internal helpers.
	// -------------------------------------------------------------------------

	/** Resolve+pin every distinct icon in the class table BEFORE a draw pass.
	 *  Synchronous LoadSynchronous is acceptable here because it runs once at
	 *  init / on an explicit full-redraw, never inside the per-tile loop. */
	void ResolveAndPinIcons();

	/** Draw one tile into an already-open FCanvas (Begin/EndDraw done by caller).
	 *  Issues the tile-bounded opaque clear quad, then the grid+Z-band-filtered
	 *  buildings clipped to the tile rect. Big-O: O(buildings in tile). */
	void DrawTileIntoCanvas(FTileId Tile, UCanvas* Canvas);

	/** Draw the geometry of one building (already computed) into the canvas.
	 *  Pure FCanvas item emission; no awaits, no allocation beyond the spline. */
	void DrawGeometry(UCanvas* Canvas, const struct FDrawGeometry& Geometry);

	/**
	 * COMMITTED Phase-A FALLBACK (SPEC Q3, §4.2): if the persistent target does
	 * NOT preserve its contents across the 2nd+ BeginDrawCanvasToRenderTarget on
	 * the CSS fork (the "lines going crazy" implicit-re-clear branch history),
	 * bypass UKismetRenderingLibrary and clear ONLY the tile rect on the render
	 * thread with an explicit scissor + ERenderTargetLoadAction::ELoad, preserving
	 * every other tile. Reachable today via the RHI dep. Marked SPIKE(Q3).
	 * Issues only the scissored clear; the building draw still goes through the
	 * FCanvas path which composites onto the ELoad-preserved target.
	 */
	void ClearTileRectViaRHI_ELoad(FTileId Tile);

	/** True once Initialize has bound a live store + atlas. */
	bool IsReady() const;
};

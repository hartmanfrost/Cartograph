#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// The mod-wide log category. Declared in this shared config header (which every
// subsystem includes — directly, or via CartographGameInstanceModule.h) so it is
// declared exactly once across all translation units, avoiding a redefinition in
// unity builds. The object is defined in CartographGameInstanceModule.cpp.
DECLARE_LOG_CATEGORY_EXTERN(LogCartograph, Display, All);

// =============================================================================
// CartographConfig.h - FROZEN CONTRACT (Agent: contracts)
// -----------------------------------------------------------------------------
// CVars, compile-time tunables, and the render-backend feature flag for the
// re-architected Cartograph. Every other subsystem includes this header for the
// authoritative world bounds, grid/tile/Z-band geometry, and the runtime knobs.
//
// World bounds are imported verbatim from the original
// CartographGameInstanceModule.h so that screen<->world math stays
// pixel-identical to the legacy renderer (SPEC Q12). The single behavioral
// change here vs. legacy is RENDER_TEXTURE_SIZE 8192 -> 4096 (SPEC 4.2): the
// uasset SizeX/Y must be matched to 4096 as a manual step. ORIGIN_UV and
// PIXEL_PER_CENTIMETER are derived constexpr off RENDER_TEXTURE_SIZE so the
// transforms stay self-consistent under the new size.
//
// Definitions of the extern CVars below live in CartographConfig.cpp.
// =============================================================================


// -----------------------------------------------------------------------------
// World bounds (verified in legacy CartographGameInstanceModule.h:28-33).
// Anisotropic on purpose: MAP_WIDTH (750000.664062) != MAP_HEIGHT (750000.0).
// The grid below derives per-axis cell counts via ceil and tolerates ragged
// edge cells (SPEC 4.1). Do NOT assume square cells anywhere downstream.
// -----------------------------------------------------------------------------
constexpr double WEST_BOUND_CENTIMETERS = -324698.832031;
constexpr double EAST_BOUND_CENTIMETERS = 425301.832031;
constexpr double NORTH_BOUND_CENTIMETERS = -375000;
constexpr double SOUTH_BOUND_CENTIMETERS = 375000;
constexpr double MAP_WIDTH_CENTIMETERS = EAST_BOUND_CENTIMETERS - WEST_BOUND_CENTIMETERS;
constexpr double MAP_HEIGHT_CENTIMETERS = SOUTH_BOUND_CENTIMETERS - NORTH_BOUND_CENTIMETERS;


// -----------------------------------------------------------------------------
// Render-target geometry.
// SPEC 4.2: RENDER_TEXTURE_SIZE dropped 8192 -> 4096 (256 MB -> 64 MB RGBA8).
// The persistent atlas uasset SizeX/Y MUST be edited to 4096 to match.
// ORIGIN_UV / PIXEL_PER_CENTIMETER derive off this value (kept self-consistent).
// -----------------------------------------------------------------------------
constexpr int RENDER_TEXTURE_SIZE = 1024 * 4;  // 4096; was 1024*8 (8192). SPEC 4.2.

constexpr double ORIGIN_UV[] = {
	-WEST_BOUND_CENTIMETERS / MAP_WIDTH_CENTIMETERS,
	-NORTH_BOUND_CENTIMETERS / MAP_HEIGHT_CENTIMETERS
};
constexpr double PIXEL_PER_CENTIMETER[] = {
	RENDER_TEXTURE_SIZE / MAP_WIDTH_CENTIMETERS,
	RENDER_TEXTURE_SIZE / MAP_HEIGHT_CENTIMETERS
};


// -----------------------------------------------------------------------------
// Tile grid (render / dirty-rect / per-tile network versioning).
// The atlas is partitioned into a fixed grid of TILE_SIZE_PX-square tiles.
// Tiles are MOD-OWNED, decoupled from World Partition cells (SPEC 4.2, Q10).
// With RENDER_TEXTURE_SIZE=4096 and TILE_SIZE_PX=256 -> 16x16 = 256 tiles.
// The bottom/right tiles can be ragged if RENDER_TEXTURE_SIZE is not an exact
// multiple of TILE_SIZE_PX; TILES_PER_AXIS uses ceil so all pixels are covered.
// -----------------------------------------------------------------------------
constexpr int TILE_SIZE_PX = 256;
constexpr int TILES_PER_AXIS = (RENDER_TEXTURE_SIZE + TILE_SIZE_PX - 1) / TILE_SIZE_PX;  // ceil
constexpr int TILE_COUNT = TILES_PER_AXIS * TILES_PER_AXIS;


// -----------------------------------------------------------------------------
// Spatial hash grid (CPU-side culling / picking / dirty-rect bookkeeping).
// Defined in centimeters with per-axis ceil cell counts (SPEC 4.1).
// ~256 m cells (25600 cm), tile-grid-aligned conceptually but maintained in
// world space. Per-axis counts differ slightly due to the anisotropic bounds.
// -----------------------------------------------------------------------------
constexpr double GRID_CELL_SIZE_CM = 25600.0;  // ~256 m
constexpr int GRID_CELLS_X = (int)((MAP_WIDTH_CENTIMETERS + GRID_CELL_SIZE_CM - 1.0) / GRID_CELL_SIZE_CM);   // ceil
constexpr int GRID_CELLS_Y = (int)((MAP_HEIGHT_CENTIMETERS + GRID_CELL_SIZE_CM - 1.0) / GRID_CELL_SIZE_CM);  // ceil
constexpr int GRID_CELL_COUNT = GRID_CELLS_X * GRID_CELLS_Y;


// -----------------------------------------------------------------------------
// Z-band index (coarse height filter, decoupled from the store; SPEC 4.1).
// Z_BAND_COUNT fixed bands spanning [Z_BAND_MIN_CM, Z_BAND_MAX_CM]. The height
// slider becomes a band-range select intersected with the spatial query.
// Z_BAND_HEIGHT_CM is the per-band thickness. A building's band is
// clamp((z - Z_BAND_MIN_CM) / Z_BAND_HEIGHT_CM, 0, Z_BAND_COUNT-1).
// -----------------------------------------------------------------------------
constexpr int Z_BAND_COUNT = 64;
constexpr double Z_BAND_MIN_CM = -100000.0;  // world floor for binning (below map floor)
constexpr double Z_BAND_MAX_CM = 540000.0;   // world ceiling for binning (above tallest builds)
constexpr double Z_BAND_HEIGHT_CM = (Z_BAND_MAX_CM - Z_BAND_MIN_CM) / Z_BAND_COUNT;  // ~10000 cm (100 m) / band


// -----------------------------------------------------------------------------
// Draw tunables (ported verbatim from legacy; keep pixel-identical, SPEC Q12).
// -----------------------------------------------------------------------------
constexpr int SPLINE_SEGMENTS = 8;                 // legacy CartographGameInstanceModule.h:40
constexpr float BOX_EXPANSION_CENTIMETERS = 300.f; // legacy CartographDataStructure.cpp:482


// -----------------------------------------------------------------------------
// Render backend selection.
//   TiledCanvas     - Phase 2 / "A": bounded per-tile FCanvas + scissored
//                     hardware clear. PRIMARY and the guaranteed TDR fix. DEFAULT.
//   SlateInstanced  - Phase 3 / "B": SLeafWidget + FSlateDrawElement::MakeCustomVerts.
//                     CVar-gated; falls back to TiledCanvas if the spike fails (Q4).
//   InstancedCapture- Phase 4 / "C": AbstractInstance + orthographic SceneCapture.
//                     CVar-gated end-state; falls back to TiledCanvas/Slate (Q5).
// Read via r.Cartograph.RenderBackend (int, default 0 == TiledCanvas).
// -----------------------------------------------------------------------------
enum class ECartographRenderBackend : uint8
{
	TiledCanvas = 0,      // Phase 2 / A - default, guaranteed floor
	SlateInstanced = 1,   // Phase 3 / B - gated
	InstancedCapture = 2, // Phase 4 / C - gated
};


// -----------------------------------------------------------------------------
// Console variables. Definitions live in CartographConfig.cpp.
// -----------------------------------------------------------------------------

/** r.Cartograph.RenderBackend (int, default 0). Maps to ECartographRenderBackend. */
extern TAutoConsoleVariable<int32> CVarCartographRenderBackend;

/**
 * r.Cartograph.FrameBudgetFraction (float, default 0.15).
 * Fraction of a frame the never-cancelled compositor may spend draining dirty
 * tiles. Frame-relative, NOT an absolute ms (SPEC 4.3, issue #10). Fed to the
 * vendored UE5Coro FTickTimeBudget.
 */
extern TAutoConsoleVariable<float> CVarCartographFrameBudgetFraction;

/**
 * r.Cartograph.TilesPerFrame (int K, default 8).
 * Upper bound on dirty tiles popped+rendered per compositor tick (the "K" in
 * PopDirtyTiles(K, ...)). The budget fraction is the soft limit; K is the hard
 * cap so a single frame can never attempt an unbounded drain.
 */
extern TAutoConsoleVariable<int32> CVarCartographTilesPerFrame;

/**
 * r.Cartograph.PersistSpatialCache (bool, default false).
 * When true, persist the grid + slim records via IFGSaveInterface to skip the
 * O(N) cold re-bucket on load. Default OFF; the save is a recomputable cache,
 * never a correctness dependency (SPEC 4.6, Q14).
 */
extern TAutoConsoleVariable<bool> CVarCartographPersistSpatialCache;

/**
 * r.Cartograph.MaxDrawablesPerFrame (int, default 128).
 * Hard cap on building drawables emitted per compositor frame; the drain loop yields
 * a full frame (NextTick) after this many, painting dense tiles progressively. This is
 * the per-frame GPU bound that prevents the Steam Deck GPU TDR: EndDrawCanvasToRenderTarget
 * only enqueues an RDG pass (the RHI coalesces a frame's passes into one submit), so the
 * frame boundary is the true submit bound; with the per-tile scissor clamping each
 * drawable's fill to one tile, per-frame fill <= cap * tile_area. Clamped >= 1.
 */
extern TAutoConsoleVariable<int32> CVarCartographMaxDrawablesPerFrame;

/**
 * r.Cartograph.RenderEnabled (bool, default true).
 * Master switch for the compositor draw. False = drain dirty tiles WITHOUT any
 * Begin/EndDraw (blank map, zero GPU work) - the escape hatch if rendering misbehaves.
 */
extern TAutoConsoleVariable<bool> CVarCartographRenderEnabled;

/**
 * r.Cartograph.DrainDebounceTicks (int, default 30).
 * Settle window: the never-cancelled compositor only drains once the TileManager dirty
 * EPOCH has been unchanged for this many consecutive ticks (the build/stream-in has
 * settled). The rework's debounce-via-epoch analogue of the original mod's
 * debounce-via-cancel - keeps the megabase join first-paint O(N) (render once after the
 * ~18k-building stream settles) instead of O(N^2) (re-render a dense tile per streamed
 * building -> render-thread backlog -> client OOM). Clamped >= 0.
 */
extern TAutoConsoleVariable<int32> CVarCartographDrainDebounceTicks;

/**
 * r.Cartograph.DrainMaxWaitTicks (int, default 1800).
 * Max-wait fallback so the debounce cannot starve under constant change: if the dirty
 * epoch keeps advancing every tick, drain anyway once this many ticks have elapsed since
 * the dirty set first became non-empty (then reset). ~30 s at 60 fps. Clamped >= 1.
 */
extern TAutoConsoleVariable<int32> CVarCartographDrainMaxWaitTicks;


// -----------------------------------------------------------------------------
// Convenience accessors (inline, header-safe).
// -----------------------------------------------------------------------------
namespace CartographConfig
{
	/** Current render backend, clamped to the valid enum range. */
	FORCEINLINE ECartographRenderBackend GetRenderBackend()
	{
		const int32 Value = CVarCartographRenderBackend.GetValueOnAnyThread();
		if (Value < 0 || Value > (int32)ECartographRenderBackend::InstancedCapture)
		{
			return ECartographRenderBackend::TiledCanvas;  // safe default
		}
		return (ECartographRenderBackend)Value;
	}
}

#include "CartographConfig.h"

// =============================================================================
// CartographConfig.cpp - CVar definitions for the re-architected Cartograph.
// Declarations live in CartographConfig.h. These are the single owning
// definitions; do not redefine elsewhere (ODR).
// =============================================================================

TAutoConsoleVariable<int32> CVarCartographRenderBackend(
	TEXT("r.Cartograph.RenderBackend"),
	0,  // ECartographRenderBackend::TiledCanvas
	TEXT("Cartograph render backend.\n")
	TEXT("  0: TiledCanvas      - per-tile FCanvas + scissored HW clear (default, guaranteed TDR fix)\n")
	TEXT("  1: SlateInstanced   - SLeafWidget + MakeCustomVerts (gated; falls back to 0 if spike fails)\n")
	TEXT("  2: InstancedCapture - AbstractInstance + orthographic SceneCapture (gated end-state)\n"),
	ECVF_Default);

TAutoConsoleVariable<float> CVarCartographFrameBudgetFraction(
	TEXT("r.Cartograph.FrameBudgetFraction"),
	0.15f,
	TEXT("Fraction of a frame the never-cancelled compositor may spend draining dirty tiles.\n")
	TEXT("Frame-relative, not an absolute ms. Fed to UE5Coro FTickTimeBudget. Clamp (0, 1]."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarCartographTilesPerFrame(
	TEXT("r.Cartograph.TilesPerFrame"),
	8,
	TEXT("Hard cap K on dirty tiles popped and rendered per compositor tick.\n")
	TEXT("The budget fraction is the soft limit; K bounds a single frame's drain."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarCartographPersistSpatialCache(
	TEXT("r.Cartograph.PersistSpatialCache"),
	false,
	TEXT("When true, persist grid + slim records via IFGSaveInterface to skip the O(N) cold re-bucket.\n")
	TEXT("Default OFF; recomputable cache, never a correctness dependency."),
	ECVF_Default);

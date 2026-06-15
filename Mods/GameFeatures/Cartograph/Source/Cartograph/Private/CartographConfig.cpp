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

TAutoConsoleVariable<int32> CVarCartographMaxDrawablesPerFrame(
	TEXT("r.Cartograph.MaxDrawablesPerFrame"),
	128,
	TEXT("Hard cap on building drawables emitted into the atlas per compositor frame.\n")
	TEXT("The drain loop co_awaits a FULL frame (NextTick) once this many drawables have\n")
	TEXT("been emitted, so a dense tile is painted progressively over several frames.\n")
	TEXT("This is the real GPU/TDR bound: EndDrawCanvasToRenderTarget only ENQUEUES an RDG\n")
	TEXT("pass (the RHI coalesces a frame's passes into one submit), so the FRAME BOUNDARY -\n")
	TEXT("not the per-tile Begin/EndDraw - is what bounds a single GPU submit. With the\n")
	TEXT("per-tile scissor clamping each drawable's fill to one 256px tile, per-frame fill is\n")
	TEXT("<= cap * tile_area, far under the Steam Deck's ~2-5s TDR window even at low clocks.\n")
	TEXT("Lower it if the Deck stutters; raise it to converge faster. Clamped >= 1."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarCartographRenderEnabled(
	TEXT("r.Cartograph.RenderEnabled"),
	true,
	TEXT("Master switch for the compositor draw. When false the drain loop still consumes\n")
	TEXT("dirty tiles (so the dirty set cannot grow unboundedly) but issues NO\n")
	TEXT("Begin/EndDrawCanvasToRenderTarget at all - the client touches the GPU zero times.\n")
	TEXT("A safe escape hatch: if rendering ever misbehaves, `r.Cartograph.RenderEnabled 0`\n")
	TEXT("in the console leaves the map blank but the game perfectly stable."),
	ECVF_Default);

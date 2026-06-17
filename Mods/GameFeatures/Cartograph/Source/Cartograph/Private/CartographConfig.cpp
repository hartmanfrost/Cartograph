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

TAutoConsoleVariable<int32> CVarCartographDrainDebounceTicks(
	TEXT("r.Cartograph.DrainDebounceTicks"),
	180,
	TEXT("Settle window (in game-thread ticks, ~3 s at 60 fps) the never-cancelled compositor\n")
	TEXT("waits before it drains the dirty-tile set. While buildings STREAM in on join (~18k on a\n")
	TEXT("megabase), every MarkDirtyForBox advances the TileManager dirty EPOCH; the compositor only\n")
	TEXT("starts draining once the epoch has been UNCHANGED for this many consecutive ticks (the\n")
	TEXT("stream has settled). Set comfortably ABOVE the gaps WITHIN a bursty join stream so a brief\n")
	TEXT("mid-stream lull does not start a drain that would then keep re-rendering tiles the rest of\n")
	TEXT("the stream re-dirties. Even if it does start early, the loop pauses the drain on the next\n")
	TEXT("streamed building (no latch), so it can never run continuously. This keeps first-paint O(N):\n")
	TEXT("a dense tile re-dirtied by hundreds of streamed buildings is rendered ONCE after settling.\n")
	TEXT("Clamped >= 0 (0 = drain immediately, no debounce)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarCartographDrainMaxWaitTicks(
	TEXT("r.Cartograph.DrainMaxWaitTicks"),
	9000,
	TEXT("Max-wait fallback (in game-thread ticks) so the debounce can never starve the drain\n")
	TEXT("under CONSTANT change. If the dirty epoch keeps advancing every tick the settle window\n")
	TEXT("never closes; once this many ticks have elapsed since the dirty set first became non-empty,\n")
	TEXT("the compositor drains anyway. MUST be comfortably LONGER than a megabase join stream-in\n")
	TEXT("(~1 min): if it fires mid-stream it latches a CONTINUOUS drain during active streaming,\n")
	TEXT("which is exactly the O(N^2)/OOM this debounce exists to avoid. 9000 ticks is ~2.5 min at\n")
	TEXT("60 fps / ~5 min at 30 fps - past any realistic stream-in. Normal play settles via the\n")
	TEXT("0.5 s debounce long before this; this is only a theoretical anti-starvation floor. Clamped >= 1."),
	ECVF_Default);

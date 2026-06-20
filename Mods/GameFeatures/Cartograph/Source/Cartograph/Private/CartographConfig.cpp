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
	2,
	TEXT("Hard cap K on dirty tiles popped per compositor drain pass.\n")
	TEXT("Each pass renders <= K tiles (sharing the MaxDrawablesPerFrame budget) then loops\n")
	TEXT("back to the settle gate, so a smaller K re-checks for new dirties more often (a\n")
	TEXT("building streaming in mid-drain pauses the paint sooner) and keeps the work between\n")
	TEXT("render-thread fences small. Lowered from 8 -> 2 with the FlushDrainFrame back-pressure:\n")
	TEXT("each fenced batch is now a couple of Begin/EndDraw passes, not up to eight, so the\n")
	TEXT("game stays responsive during a megabase first-paint."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarCartographPersistSpatialCache(
	TEXT("r.Cartograph.PersistSpatialCache"),
	false,
	TEXT("When true, persist grid + slim records via IFGSaveInterface to skip the O(N) cold re-bucket.\n")
	TEXT("Default OFF; recomputable cache, never a correctness dependency."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarCartographMaxDrawablesPerFrame(
	TEXT("r.Cartograph.MaxDrawablesPerFrame"),
	64,
	TEXT("Hard cap on building drawables emitted into the atlas per compositor frame.\n")
	TEXT("Once this many drawables have been emitted the drain co_awaits FlushDrainFrame: it\n")
	TEXT("yields a FULL frame AND fences the render thread, so a dense tile paints progressively\n")
	TEXT("over several frames with at most one frame's batch in flight on the GPU at a time.\n")
	TEXT("Two bounds in one: (1) GPU/TDR - EndDrawCanvasToRenderTarget only ENQUEUES an RDG pass\n")
	TEXT("(the RHI coalesces a frame's passes into one submit), so the FRAME BOUNDARY bounds a\n")
	TEXT("single submit, and the per-tile scissor clamps each drawable's fill to one 256px tile,\n")
	TEXT("keeping per-frame fill <= cap * tile_area, far under the Deck's ~2-5s TDR window;\n")
	TEXT("(2) MEMORY - the fence in FlushDrainFrame stops the game thread enqueuing the next\n")
	TEXT("frame's passes before the slow Deck GPU has retired the last, which is what otherwise\n")
	TEXT("grew the process to ~16 GB and got it OOM-killed mid-paint. Lowered 128 -> 64 so each\n")
	TEXT("fenced batch is smaller and the game stays responsive. Lower if the Deck stutters;\n")
	TEXT("raise to converge faster (memory stays bounded either way). Clamped >= 1."),
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

TAutoConsoleVariable<int32> CVarCartographMaxDrawablesPerDrain(
	TEXT("r.Cartograph.MaxDrawablesPerDrain"),
	6000,
	TEXT("LEAK-CALIBRATED SAFETY CAP. Max building drawables EMITTED into the atlas per drain session.\n")
	TEXT("The megabase OOM growth tracks drawables DRAWN (~0.86 MB each per the DRAINMEM logs), so capping\n")
	TEXT("drawables bounds the worst case directly: 6000 * 0.86 MB ~= 5 GB over a ~3.9 GB baseline ~= 9 GB,\n")
	TEXT("safely under the Deck's ~11.8 GB. Unlike a Begin/Draw-call cap, empty tiles (0 drawables) do not\n")
	TEXT("burn the budget, so the dense base paints. Remaining tiles still drain (just not drawn). If the\n")
	TEXT("GPU-synced fence holds the memory flat up to this cap, raise/remove it next build. 0 = uncapped.\n")
	TEXT("Clamped >= 0."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarCartographMaxBeginDrawsPerDrain(
	TEXT("r.Cartograph.MaxBeginDrawsPerDrain"),
	0,
	TEXT("SECONDARY/LEGACY cap on BeginDrawCanvasToRenderTarget calls per drain (0 = OFF, the default now).\n")
	TEXT("Superseded by r.Cartograph.MaxDrawablesPerDrain, which caps the actual leak axis (drawables) rather\n")
	TEXT("than Begin/Draw calls that empty tiles inflate. Kept for manual diagnostics.\n")
	TEXT("--- legacy notes ---\n")
	TEXT("Max BeginDrawCanvasToRenderTarget calls per drain session.\n")
	TEXT("Once hit, remaining tiles are still popped/gathered/consumed (the dirty set drains\n")
	TEXT("identically) but the canvas draw is SKIPPED, so memory cannot run away to the ~16 GB\n")
	TEXT("megabase OOM. Doubles as the canvas-vs-gather discriminator: with the DRAINMEM log, if\n")
	TEXT("used memory rises to the cap then FLATTENS across the no-draw tail -> the BeginDraw/canvas\n")
	TEXT("path is the leak; if it keeps climbing through the tail -> gather/store/net is. 0 = uncapped\n")
	TEXT("(old behavior, OOMs on a megabase). Clamped >= 0."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarCartographFlushAfterNDraws(
	TEXT("r.Cartograph.FlushAfterNDraws"),
	0,
	TEXT("DIAGNOSTIC RECLAIM PROBE. When > 0, FlushRenderingCommands() after every N BeginDraw calls\n")
	TEXT("to force the render thread + RHI deferred-deletion to run. If the per-open growth is merely\n")
	TEXT("reclaim LAG it gets bounded; if it is a true per-bind driver/pool leak it will NOT help\n")
	TEXT("(distinguishes leak vs lag). Off by default so the baseline DRAINMEM curve is unconfounded.\n")
	TEXT("Clamped >= 0."),
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

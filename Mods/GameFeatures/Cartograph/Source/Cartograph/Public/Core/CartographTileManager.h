#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"

// =============================================================================
// CartographTileManager.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "tilemanager" agent (CartographTileManager.cpp).
// -----------------------------------------------------------------------------
// Owns the monotonic dirty-tile set, the per-tile uint64 version counters, and
// tile<->rect helpers (SPEC 4.2, 4.3, 4.4).
//
// Build hooks call MarkDirtyForBox (O(1) per touched tile). The never-cancelled
// compositor calls PopDirtyTiles(K, ...) each tick. The dirty set only ever
// SHRINKS during a pass; a tile re-dirtied mid-pass is coalesced by its bit
// (dirtied 100x -> renders once). On the server, BumpVersion is called per
// touched tile so clients can pull deltas by version.
// =============================================================================
class CARTOGRAPH_API FCartographTileManager
{
public:
	FCartographTileManager() = default;
	~FCartographTileManager() = default;

	FCartographTileManager(const FCartographTileManager&) = delete;
	FCartographTileManager& operator=(const FCartographTileManager&) = delete;

	/** Allocate the TILE_COUNT dirty bitset + version array. Call once at init. */
	void Initialize();

	// -------------------------------------------------------------------------
	// Dirty-tile bookkeeping (monotonic).
	// -------------------------------------------------------------------------

	/**
	 * Mark every tile a SCREEN-space box overlaps as dirty.
	 * @param ScreenBox  pixel-space bounds (e.g. FDrawGeometry::VisualBox).
	 * Big-O: O(tiles-covered). Sets bits; never clears (monotonic).
	 */
	void MarkDirtyForBox(const FBox2f& ScreenBox);

	/** Mark a single tile dirty by id. O(1). */
	void MarkDirty(FTileId Tile);

	/** Mark all tiles dirty (the #10 full-redraw button / SetFullRedraw). O(tiles). */
	void MarkAllDirty();

	/**
	 * Pop up to K dirty tiles into OutTiles, clearing their dirty bits, ordered
	 * by AoI priority if an AoI was set (SetAoI), else arbitrary. Returns the
	 * count popped. The compositor renders these and they leave the dirty set;
	 * re-dirtying during the pass simply re-sets the bit for a later tick.
	 * Big-O: O(K) amortized with a maintained cursor; worst case O(tiles).
	 * OutTiles is reset by the callee.
	 */
	int32 PopDirtyTiles(int32 K, TArray<FTileId>& OutTiles);

	/** Number of currently-dirty tiles. O(1) if maintained, else O(tiles). */
	int32 DirtyNum() const;

	/** True iff any tile is dirty. */
	bool HasDirty() const;

	/**
	 * Monotonic "dirty epoch": advanced by EVERY Mark* that actually sets a previously
	 * clear bit (the single choke point MarkDirtyInternal). The compositor reads this to
	 * DEBOUNCE its drain - it only paints once the epoch has been STABLE for N ticks (the
	 * stream/build burst has settled), the rework's analogue of the original mod's
	 * debounce-via-cancel. O(1). Wraps harmlessly (only equality across consecutive ticks
	 * is tested, never magnitude). Starts at 0; the first mark makes it non-zero.
	 */
	uint64 GetDirtyEpoch() const;

	// -------------------------------------------------------------------------
	// Versioning (server-side). Monotonic uint64 per tile (SPEC Q8: counter,
	// NOT a content hash on the hot path).
	// -------------------------------------------------------------------------

	/** Increment a tile's version. O(1). Called on the server per touched tile. */
	void BumpVersion(FTileId Tile);

	/** Read a tile's current version. O(1). Zero means "never touched". */
	FTileVersion GetVersion(FTileId Tile) const;

	// -------------------------------------------------------------------------
	// Area-of-interest (drives PopDirtyTiles ordering and network pull order).
	// The AoI is the UI VIEWPORT rect, explicitly NOT the world-streaming AoI.
	// -------------------------------------------------------------------------

	/**
	 * Set the current viewport AoI in SCREEN space. PopDirtyTiles will then
	 * prioritize tiles inside/near this rect (visible map converges first).
	 */
	void SetAoI(const FBox2f& ScreenViewportBox);

	/** Append all tile ids overlapping the current AoI to OutTiles. */
	void GetAoITiles(TArray<FTileId>& OutTiles) const;

	/** Append all tile ids overlapping an arbitrary screen box to OutTiles. */
	static void GetTilesForBox(const FBox2f& ScreenBox, TArray<FTileId>& OutTiles);

	// -------------------------------------------------------------------------
	// Tile <-> rect helpers (thin wrappers over CartographCoords for callers
	// that hold only the manager).
	// -------------------------------------------------------------------------

	/** Pixel rect of a tile in the atlas (ragged edges clamped). */
	static FBox2f TileRect(FTileId Tile);

	/** Tile id containing a screen point (clamped). */
	static FTileId TileAtScreen(const FVector2f& ScreenPos);

private:
	// Implementation (owning agent): TBitArray DirtyTiles sized TILE_COUNT,
	// TArray<FTileVersion> Versions sized TILE_COUNT, a cached AoI FBox2f, and
	// an optional dirty-count cache + scan cursor for amortized PopDirtyTiles.

	/** Set one tile's dirty bit, maintaining DirtyCount. Tile must be in range. */
	void MarkDirtyInternal(FTileId Tile);

	/** Monotonic dirty-tile bitset, one bit per tile, sized TILE_COUNT. Bits are
	 *  set by Mark* and only cleared by PopDirtyTiles (the set only shrinks
	 *  during a single drain - SPEC 4.3). */
	TBitArray<> DirtyTiles;

	/** Per-tile monotonic uint64 version counters, sized TILE_COUNT (SPEC Q8). */
	TArray<FTileVersion> Versions;

	/** Cached viewport AoI rect in SCREEN space (px); valid iff bHasAoI. */
	FBox2f AoIBox = FBox2f(ForceInit);

	/** Whether a viewport AoI has been set (drives PopDirtyTiles ordering). */
	bool bHasAoI = false;

	/** Maintained count of set bits in DirtyTiles so DirtyNum/HasDirty are O(1). */
	int32 DirtyCount = 0;

	/** Linear scan cursor for the non-AoI PopDirtyTiles drain (amortized O(K)). */
	int32 PopCursor = 0;

	/** Monotonic dirty epoch (see GetDirtyEpoch). Bumped in MarkDirtyInternal on every
	 *  0->1 bit transition so each MarkDirtyForBox during the stream-in advances it; the
	 *  compositor debounces its drain on this being stable across ticks. */
	uint64 DirtyEpoch = 0;
};

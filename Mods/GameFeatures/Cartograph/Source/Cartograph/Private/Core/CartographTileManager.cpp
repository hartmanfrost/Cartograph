#include "Core/CartographTileManager.h"

#include "CartographConfig.h"               // TILE_SIZE_PX / TILES_PER_AXIS / TILE_COUNT / RENDER_TEXTURE_SIZE
#include "CartographGameInstanceModule.h"   // CARTO_LOG / CARTO_LOG_ERROR macros + LogCartograph

// =============================================================================
// CartographTileManager.cpp - implementation of the mod-owned dirty-tile set,
// per-tile uint64 version counters, and AoI-prioritized PopDirtyTiles drain.
//
// Backing storage (matches the contract header's private-section note):
//   - DirtyTiles : TBitArray sized TILE_COUNT (one bit per tile, monotonic-set).
//   - Versions   : TArray<FTileVersion> sized TILE_COUNT (O(1) bump on dirty).
//   - AoIBox     : cached viewport rect in SCREEN space (px), drives Pop order.
//   - DirtyCount : maintained count so DirtyNum/HasDirty are O(1) on the hot
//                  path (the compositor polls HasDirty every tick).
//   - PopCursor  : maintained scan cursor so the steady-state PopDirtyTiles is
//                  O(K) amortized rather than O(TILE_COUNT) per tick.
//
// Everything here keys on the SCREEN-space RENDER_TEXTURE_SIZE atlas pixel space
// and uses the FROZEN CartographCoords helpers so tile<->rect math is identical
// to every other subsystem (SPEC 4.2, 4.4). All tile geometry comes from
// CartographConfig (TILE_SIZE_PX / TILES_PER_AXIS / TILE_COUNT).
// =============================================================================


// -----------------------------------------------------------------------------
// Internal helpers (translation-unit local).
// -----------------------------------------------------------------------------
namespace
{
	/** True iff Tile is a representable id within the allocated grid. */
	FORCEINLINE bool IsValidTile(FTileId Tile)
	{
		return Tile != InvalidTileId && (int32)Tile < TILE_COUNT;
	}

	/**
	 * Clamp a screen-space box to the atlas, then resolve the inclusive tile
	 * range [min,max] it covers on each axis. Returns false if the box is empty
	 * or lies entirely outside the atlas (nothing to cover).
	 */
	bool ResolveTileRange(const FBox2f& ScreenBox, int32& OutMinX, int32& OutMinY,
		int32& OutMaxX, int32& OutMaxY)
	{
		if (!ScreenBox.bIsValid)
		{
			return false;
		}

		// Normalize a possibly-inverted box (Min > Max) before clamping so a
		// degenerate input still covers the tiles between its true endpoints -
		// matching FCartographSpatialGrid::CellRangeFromBox's defensive reorder.
		const float RawMinX = FMath::Min(ScreenBox.Min.X, ScreenBox.Max.X);
		const float RawMaxX = FMath::Max(ScreenBox.Min.X, ScreenBox.Max.X);
		const float RawMinY = FMath::Min(ScreenBox.Min.Y, ScreenBox.Max.Y);
		const float RawMaxY = FMath::Max(ScreenBox.Min.Y, ScreenBox.Max.Y);

		// Clamp to the atlas pixel space before integer-binning so an out-of-range
		// box (negative or > RENDER_TEXTURE_SIZE) can never index a phantom tile.
		const float AtlasMax = (float)RENDER_TEXTURE_SIZE;
		const float MinX = FMath::Clamp(RawMinX, 0.f, AtlasMax);
		const float MinY = FMath::Clamp(RawMinY, 0.f, AtlasMax);
		const float MaxX = FMath::Clamp(RawMaxX, 0.f, AtlasMax);
		const float MaxY = FMath::Clamp(RawMaxY, 0.f, AtlasMax);

		// Box wholly outside the atlas (entirely left/above origin or right/below
		// the far edge): nothing to cover. Tested on the un-clamped raw extents.
		if (RawMaxX < 0.f || RawMaxY < 0.f || RawMinX > AtlasMax || RawMinY > AtlasMax)
		{
			return false;
		}

		OutMinX = FMath::Clamp((int32)(MinX / TILE_SIZE_PX), 0, TILES_PER_AXIS - 1);
		OutMinY = FMath::Clamp((int32)(MinY / TILE_SIZE_PX), 0, TILES_PER_AXIS - 1);
		// The max edge is exclusive in pixel terms; a box exactly on a tile
		// boundary (e.g. Max.X == k*TILE_SIZE_PX) should NOT pull in the next
		// tile, so bias the max coordinate down by an epsilon before binning.
		OutMaxX = FMath::Clamp((int32)(FMath::Max(MaxX - KINDA_SMALL_NUMBER, MinX) / TILE_SIZE_PX), 0, TILES_PER_AXIS - 1);
		OutMaxY = FMath::Clamp((int32)(FMath::Max(MaxY - KINDA_SMALL_NUMBER, MinY) / TILE_SIZE_PX), 0, TILES_PER_AXIS - 1);

		return true;
	}
}


// -----------------------------------------------------------------------------
// Initialize - allocate the dirty bitset + version array. Idempotent.
// -----------------------------------------------------------------------------
void FCartographTileManager::Initialize()
{
	// TBitArray<>::Init(false, N) sizes the bitset to N bits, all clear.
	DirtyTiles.Init(false, TILE_COUNT);

	// One uint64 version per tile, zero == "never touched".
	Versions.SetNumZeroed(TILE_COUNT);

	DirtyCount = 0;
	PopCursor = 0;
	DirtyEpoch = 0;
	bHasAoI = false;
	AoIBox = FBox2f(ForceInit);

	CARTO_LOG("TileManager initialized: %d tiles (%dx%d, %dpx/tile, atlas %dpx)",
		TILE_COUNT, TILES_PER_AXIS, TILES_PER_AXIS, TILE_SIZE_PX, RENDER_TEXTURE_SIZE);
}


// -----------------------------------------------------------------------------
// Dirty-tile bookkeeping.
// -----------------------------------------------------------------------------
void FCartographTileManager::MarkDirtyForBox(const FBox2f& ScreenBox)
{
	int32 MinX, MinY, MaxX, MaxY;
	if (!ResolveTileRange(ScreenBox, MinX, MinY, MaxX, MaxY))
	{
		return;
	}

	for (int32 TileY = MinY; TileY <= MaxY; ++TileY)
	{
		for (int32 TileX = MinX; TileX <= MaxX; ++TileX)
		{
			MarkDirtyInternal(CartographCoords::TileId(TileX, TileY));
		}
	}
}


void FCartographTileManager::MarkDirty(FTileId Tile)
{
	if (!IsValidTile(Tile))
	{
		CARTO_LOG_ERROR("MarkDirty: out-of-range tile id %u (TILE_COUNT=%d)", Tile, TILE_COUNT);
		return;
	}
	MarkDirtyInternal(Tile);
}


void FCartographTileManager::MarkAllDirty()
{
	if (DirtyTiles.Num() != TILE_COUNT)
	{
		// Defensive: MarkAllDirty before Initialize would silently no-op.
		CARTO_LOG_ERROR("MarkAllDirty before Initialize (bitset has %d bits)", DirtyTiles.Num());
		return;
	}

	DirtyTiles.Init(true, TILE_COUNT);
	DirtyCount = TILE_COUNT;
	// A full-redraw should let the AoI ordering converge the visible map first,
	// so restart the linear cursor; the AoI pass in PopDirtyTiles handles the
	// visible tiles ahead of the linear drain regardless of where the cursor is.
	PopCursor = 0;
	// MarkAllDirty sets bits directly (not via MarkDirtyInternal), so bump the epoch here
	// too. SetFullRedraw -> MarkAllDirty on join must register as a change so the debounce
	// re-arms and the converged state is drained once the join settles.
	++DirtyEpoch;
}


// -----------------------------------------------------------------------------
// PopDirtyTiles - AoI-prioritized, bounded drain.
//
// Order of selection:
//   1. If an AoI is set, every dirty tile overlapping the AoI rect (the visible
//      map converges first - SPEC 4.3 backpressure / 4.4 per-AoI pull order).
//   2. Then the rest of the dirty set in cursor order (amortized O(K)).
// Popped tiles have their bit cleared immediately; a tile re-dirtied later in
// the same frame (by a concurrent build hook) simply re-sets its bit for a
// future tick - the set only ever shrinks during a single drain (SPEC 4.3).
// -----------------------------------------------------------------------------
int32 FCartographTileManager::PopDirtyTiles(int32 K, TArray<FTileId>& OutTiles)
{
	OutTiles.Reset();

	if (K <= 0 || DirtyCount == 0)
	{
		return 0;
	}

	OutTiles.Reserve(FMath::Min(K, DirtyCount));

	// ---- Pass 1: AoI-overlapping dirty tiles first (visible map converges). --
	if (bHasAoI)
	{
		int32 MinX, MinY, MaxX, MaxY;
		if (ResolveTileRange(AoIBox, MinX, MinY, MaxX, MaxY))
		{
			for (int32 TileY = MinY; TileY <= MaxY && OutTiles.Num() < K; ++TileY)
			{
				for (int32 TileX = MinX; TileX <= MaxX && OutTiles.Num() < K; ++TileX)
				{
					const FTileId Tile = CartographCoords::TileId(TileX, TileY);
					if (DirtyTiles[Tile])
					{
						DirtyTiles[Tile] = false;
						--DirtyCount;
						OutTiles.Add(Tile);
					}
				}
			}
		}

		if (OutTiles.Num() >= K || DirtyCount == 0)
		{
			return OutTiles.Num();
		}
	}

	// ---- Pass 2: drain the remaining dirty set in cursor order. --------------
	// PopCursor is a maintained scan position so steady-state pops are O(K)
	// rather than O(TILE_COUNT). It can wrap once; we bound the total scan to
	// TILE_COUNT iterations so we never spin even if the cursor is stale.
	if (PopCursor >= TILE_COUNT)
	{
		PopCursor = 0;
	}

	int32 Scanned = 0;
	while (OutTiles.Num() < K && DirtyCount > 0 && Scanned < TILE_COUNT)
	{
		const FTileId Tile = (FTileId)PopCursor;
		if (DirtyTiles[Tile])
		{
			DirtyTiles[Tile] = false;
			--DirtyCount;
			OutTiles.Add(Tile);
		}

		++PopCursor;
		if (PopCursor >= TILE_COUNT)
		{
			PopCursor = 0;
		}
		++Scanned;
	}

	return OutTiles.Num();
}


int32 FCartographTileManager::DirtyNum() const
{
	return DirtyCount;
}


bool FCartographTileManager::HasDirty() const
{
	return DirtyCount > 0;
}


uint64 FCartographTileManager::GetDirtyEpoch() const
{
	return DirtyEpoch;
}


// -----------------------------------------------------------------------------
// Versioning (server-side). Monotonic uint64 counter per tile (SPEC Q8).
// -----------------------------------------------------------------------------
void FCartographTileManager::BumpVersion(FTileId Tile)
{
	if (!IsValidTile(Tile))
	{
		CARTO_LOG_ERROR("BumpVersion: out-of-range tile id %u (TILE_COUNT=%d)", Tile, TILE_COUNT);
		return;
	}
	if (Versions.Num() != TILE_COUNT)
	{
		CARTO_LOG_ERROR("BumpVersion before Initialize (versions has %d entries)", Versions.Num());
		return;
	}
	++Versions[Tile];
}


FTileVersion FCartographTileManager::GetVersion(FTileId Tile) const
{
	if (!IsValidTile(Tile) || Versions.Num() != TILE_COUNT)
	{
		return 0;
	}
	return Versions[Tile];
}


// -----------------------------------------------------------------------------
// Area-of-interest (drives PopDirtyTiles ordering and network pull order).
// -----------------------------------------------------------------------------
void FCartographTileManager::SetAoI(const FBox2f& ScreenViewportBox)
{
	if (ScreenViewportBox.bIsValid)
	{
		AoIBox = ScreenViewportBox;
		bHasAoI = true;
	}
	else
	{
		// An invalid/empty viewport box clears the AoI: PopDirtyTiles falls back
		// to arbitrary cursor order (the contract's "else arbitrary").
		bHasAoI = false;
		AoIBox = FBox2f(ForceInit);
	}
}


void FCartographTileManager::GetAoITiles(TArray<FTileId>& OutTiles) const
{
	if (!bHasAoI)
	{
		return;
	}
	GetTilesForBox(AoIBox, OutTiles);
}


void FCartographTileManager::GetTilesForBox(const FBox2f& ScreenBox, TArray<FTileId>& OutTiles)
{
	int32 MinX, MinY, MaxX, MaxY;
	if (!ResolveTileRange(ScreenBox, MinX, MinY, MaxX, MaxY))
	{
		return;
	}

	OutTiles.Reserve(OutTiles.Num() + (MaxX - MinX + 1) * (MaxY - MinY + 1));
	for (int32 TileY = MinY; TileY <= MaxY; ++TileY)
	{
		for (int32 TileX = MinX; TileX <= MaxX; ++TileX)
		{
			OutTiles.Add(CartographCoords::TileId(TileX, TileY));
		}
	}
}


// -----------------------------------------------------------------------------
// Tile <-> rect helpers (thin wrappers over the FROZEN CartographCoords math so
// callers holding only the manager don't need to include the coord helpers).
// -----------------------------------------------------------------------------
FBox2f FCartographTileManager::TileRect(FTileId Tile)
{
	return CartographCoords::ScreenRectFromTile(Tile);
}


FTileId FCartographTileManager::TileAtScreen(const FVector2f& ScreenPos)
{
	return CartographCoords::TileIdFromScreen(ScreenPos);
}


// -----------------------------------------------------------------------------
// Private helper: set one tile's dirty bit, maintaining the cached count.
// Caller guarantees Tile is in [0, TILE_COUNT). The single choke point for every
// dirty-set write, so the pre-Initialize guard lives here once.
// -----------------------------------------------------------------------------
void FCartographTileManager::MarkDirtyInternal(FTileId Tile)
{
	if (DirtyTiles.Num() != TILE_COUNT)
	{
		// Bitset not yet allocated (Mark* before Initialize). Indexing here would
		// be out-of-bounds; report once and bail rather than crash.
		CARTO_LOG_ERROR("MarkDirty before Initialize (bitset has %d bits)", DirtyTiles.Num());
		return;
	}

	if (!DirtyTiles[Tile])
	{
		DirtyTiles[Tile] = true;
		++DirtyCount;
		// Advance the dirty epoch on every NEW dirty tile. This is the single write choke
		// point for the bitset, so every MarkDirtyForBox during the ~18k-building stream-in
		// bumps it; the compositor debounces its drain on this being stable for N ticks (the
		// stream settled) - keeping the megabase first-paint O(N), not O(N^2).
		++DirtyEpoch;
	}
}

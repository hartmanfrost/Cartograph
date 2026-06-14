#include "Core/CartographZBandIndex.h"

#include "CartographConfig.h"
#include "CartographGameInstanceModule.h"  // CARTO_LOG_* macros (LogCartograph)

// =============================================================================
// CartographZBandIndex.cpp - coarse Z-band height index (SPEC 4.1).
// -----------------------------------------------------------------------------
// Z_BAND_COUNT fixed bands over [Z_BAND_MIN_CM, Z_BAND_MAX_CM], DECOUPLED from
// the SoA store so the master columns never sort. A handle lives in exactly one
// band (the band of its Pos.Z).
//
//   Insert  = O(1) push into one band.
//   Remove  = O(1) swap-remove within one band.
//   Query   = O(bands-in-range + hits), intersected by the caller with the
//             spatial-grid result. Bands visited low->high (painters' order).
//
// This deletes the legacy operator<=> Z-sort coupling that forced unstable
// positions, and turns OnZFilterUpdated from an O(N) full re-walk + 256 MB
// clear into O(bands + hits) (root cause: client-full-rebuild-on-zfilter).
// =============================================================================


void FCartographZBandIndex::Initialize()
{
	static_assert(Z_BAND_COUNT > 0, "Z_BAND_COUNT must be positive");

	Bands.Empty(Z_BAND_COUNT);
	Bands.SetNum(Z_BAND_COUNT);

	CARTO_LOG("ZBandIndex::Initialize: %d bands over [%.0f, %.0f] cm, %.0f cm/band",
		Z_BAND_COUNT, Z_BAND_MIN_CM, Z_BAND_MAX_CM, Z_BAND_HEIGHT_CM);
}


void FCartographZBandIndex::Insert(FBuildingHandle Handle, float WorldZ)
{
	if (Bands.Num() == 0)
	{
		CARTO_LOG_ERROR("ZBandIndex::Insert before Initialize(); dropping handle (Slot=%u)", Handle.Slot);
		return;
	}

	// ZBandFromWorldZ clamps into [0, Z_BAND_COUNT-1], so a building below the
	// binning floor or above the ceiling lands in the edge band rather than
	// indexing out of range. O(1).
	const int32 Band = CartographCoords::ZBandFromWorldZ(WorldZ);
	Bands[Band].Add(Handle);
}


void FCartographZBandIndex::Remove(FBuildingHandle Handle, float WorldZ)
{
	if (Bands.Num() == 0)
	{
		return;
	}

	// Same Z => same band as Insert (contract). Swap-remove the single
	// occurrence: O(1), unordered bucket - NO order preservation, NO scan of
	// other bands.
	const int32 Band = CartographCoords::ZBandFromWorldZ(WorldZ);
	FBandArray& Bucket = Bands[Band];
	const int32 Found = Bucket.IndexOfByKey(Handle);
	if (Found != INDEX_NONE)
	{
		// Explicit (Index, Count, EAllowShrinking) overload - verified on the
		// fork via the IndexOfByKey+RemoveAt pattern in FGSignSubsystem.h:161-164.
		Bucket.RemoveAtSwap(Found, 1, EAllowShrinking::No);
	}
}


void FCartographZBandIndex::QueryBandRange(float MinZ, float MaxZ, TArray<FBuildingHandle>& OutHandles) const
{
	// Delegate to the inline visitor form (single source of truth for the
	// low->high band walk). OutHandles is appended to, not reset (contract);
	// handles arrive low band first (painters' order).
	QueryBandRange(MinZ, MaxZ, [&OutHandles](FBuildingHandle Handle)
	{
		OutHandles.Add(Handle);
	});
}


int32 FCartographZBandIndex::NumEntries() const
{
	// Each handle lives in exactly one band, so this is the true live count
	// (no multi-band duplicates, unlike the spatial grid). O(bands).
	int32 Total = 0;
	for (const FBandArray& Bucket : Bands)
	{
		Total += Bucket.Num();
	}
	return Total;
}


void FCartographZBandIndex::Reset()
{
	// Clear every band but keep the per-band and flat allocations for reuse on
	// the next gather/rebuild. O(bands).
	for (FBandArray& Bucket : Bands)
	{
		Bucket.Reset();
	}
}

#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"

// =============================================================================
// CartographZBandIndex.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "zband" agent (CartographZBandIndex.cpp).
// -----------------------------------------------------------------------------
// Coarse height (Z) filter, DECOUPLED from the store (SPEC 4.1). This is the
// decision that lets the master columns never sort: the legacy operator<=>
// Z-sort coupling is deleted. The height slider becomes a band-range select
// intersected with the spatial query, turning OnZFilterUpdated from O(N) full
// re-walk + 256 MB clear into O(bands + hits).
//
// Z_BAND_COUNT fixed bands (CartographConfig). A handle lives in exactly one
// band (the band of its Pos.Z). Z is in WORLD centimeters.
// =============================================================================
class CARTOGRAPH_API FCartographZBandIndex
{
public:
	FCartographZBandIndex() = default;
	~FCartographZBandIndex() = default;

	FCartographZBandIndex(const FCartographZBandIndex&) = delete;
	FCartographZBandIndex& operator=(const FCartographZBandIndex&) = delete;

	/** Allocate the Z_BAND_COUNT bands. Call once at init. */
	void Initialize();

	/**
	 * Insert Handle into the band for WorldZ. Big-O: O(1).
	 * Band index = CartographCoords::ZBandFromWorldZ(WorldZ).
	 */
	void Insert(FBuildingHandle Handle, float WorldZ);

	/**
	 * Remove Handle from the band for WorldZ. Pass the SAME Z used at Insert.
	 * Big-O: O(1) swap-remove within the band.
	 */
	void Remove(FBuildingHandle Handle, float WorldZ);

	/**
	 * Visit every handle whose band overlaps [MinZ, MaxZ] (world cm).
	 * Visitor: void(FBuildingHandle). Big-O: O(bands-in-range + hits).
	 * Drawing back-to-front: bands are visited low->high so painters' order is
	 * exactly what Phase B's instance order needs (SPEC 4.2).
	 */
	template<typename FuncType>
	void QueryBandRange(float MinZ, float MaxZ, FuncType&& Visitor) const;

	/**
	 * Append every handle in [MinZ, MaxZ] to OutHandles (low band first).
	 * OutHandles is appended to, not reset. Big-O: O(bands-in-range + hits).
	 */
	void QueryBandRange(float MinZ, float MaxZ, TArray<FBuildingHandle>& OutHandles) const;

	/** Total handles across all bands. */
	int32 NumEntries() const;

	/** Clear all bands; keeps allocation. */
	void Reset();

private:
	// One bucket per band (Z_BAND_COUNT of them), each a small unordered array
	// of handles. A handle lives in exactly ONE band (the band of its Pos.Z),
	// so the master SoA columns never sort and stay positionally stable
	// (SPEC 4.1, deletes z-sort-coupling). Bucket is inline-allocated: a band
	// covering empty sky needs no heap, while a busy ground band spills.
	using FBandArray = TArray<FBuildingHandle, TInlineAllocator<8>>;

	/** Flat band store; empty until Initialize(). */
	TArray<FBandArray> Bands;

	/**
	 * Resolve [MinZ, MaxZ] (world cm) to the inclusive band index span,
	 * clamped into [0, Z_BAND_COUNT-1]. Always returns Lo <= Hi so callers can
	 * iterate the span unconditionally. Returns false iff the index is empty.
	 */
	FORCEINLINE bool BandSpanFromRange(float MinZ, float MaxZ, int32& OutLo, int32& OutHi) const
	{
		if (Bands.Num() == 0)
		{
			return false;
		}

		// Normalize an inverted [Max, Min] caller range before binning.
		float Lo = MinZ;
		float Hi = MaxZ;
		if (Lo > Hi)
		{
			Swap(Lo, Hi);
		}

		// ZBandFromWorldZ already clamps to [0, Z_BAND_COUNT-1], so out-of-band
		// queries fold onto the floor/ceiling band rather than missing.
		OutLo = CartographCoords::ZBandFromWorldZ(Lo);
		OutHi = CartographCoords::ZBandFromWorldZ(Hi);
		return true;
	}
};


// -----------------------------------------------------------------------------
// QueryBandRange (visitor form) - header-inline so callers (compositor / Z
// slider) can pass a lambda with no TArray allocation. Bands are visited
// low->high so handles arrive in painters' (back-to-front) order, which is
// exactly what Phase B's instance ordering needs (SPEC 4.2). Within a band the
// order is unspecified (unordered bucket) - acceptable because intra-band Z
// spread is one band thickness (~100 m) and finer ordering is the renderer's job.
// -----------------------------------------------------------------------------
template<typename FuncType>
void FCartographZBandIndex::QueryBandRange(float MinZ, float MaxZ, FuncType&& Visitor) const
{
	int32 LoBand, HiBand;
	if (!BandSpanFromRange(MinZ, MaxZ, LoBand, HiBand))
	{
		return;
	}

	for (int32 Band = LoBand; Band <= HiBand; ++Band)
	{
		const FBandArray& Bucket = Bands[Band];
		for (const FBuildingHandle& Handle : Bucket)
		{
			Visitor(Handle);
		}
	}
}

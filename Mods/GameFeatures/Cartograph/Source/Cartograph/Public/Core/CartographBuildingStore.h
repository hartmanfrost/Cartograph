#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"

// =============================================================================
// CartographBuildingStore.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "store" agent (CartographBuildingStore.cpp).
// -----------------------------------------------------------------------------
// SoA / ECS column store keyed by FBuildingHandle (SPEC 4.1).
// TSparseArray-backed: free-list slot reuse, positions NEVER shift (O(1)
// Add/RemoveAt). A per-slot generation counter makes recycled handles ABA-safe.
//
// This replaces the legacy (Z-sorted TArray<FBuildingData> + index redirector)
// entirely. The store NEVER sorts: Z-filtering lives in the separate
// FCartographZBandIndex; spatial culling lives in FCartographSpatialGrid.
//
// Typed side-tables (splines/wires/beams) are owned here so a record's
// ExtraIndex resolves through this one object. A belt never pays for a
// rectangle cache (SPEC 4.1).
// =============================================================================
class CARTOGRAPH_API FCartographBuildingStore
{
public:
	FCartographBuildingStore() = default;
	~FCartographBuildingStore() = default;

	// Non-copyable: the store is a large unique authority. Move is allowed.
	FCartographBuildingStore(const FCartographBuildingStore&) = delete;
	FCartographBuildingStore& operator=(const FCartographBuildingStore&) = delete;
	FCartographBuildingStore(FCartographBuildingStore&&) = default;
	FCartographBuildingStore& operator=(FCartographBuildingStore&&) = default;

	// -------------------------------------------------------------------------
	// Mutation. O(1) amortized; this is the hot path called from build hooks.
	// -------------------------------------------------------------------------

	/**
	 * Insert a slim record. Returns a stable generational handle.
	 * Big-O: O(1) amortized (TSparseArray free-list slot, no shift).
	 * The caller is responsible for inserting the returned handle into the grid
	 * and Z-band index; the store does NOT touch them.
	 */
	FBuildingHandle Add(const FBuildingRecord& Record);

	/**
	 * Remove the building at Handle. No-op (returns false) if Handle is stale.
	 * Big-O: O(1) (TSparseArray RemoveAt + side-table free-list push; no scan,
	 * no redirector walk, no shift). Bumps the slot generation so the freed slot
	 * cannot be aliased by an old handle.
	 */
	bool Remove(FBuildingHandle Handle);

	// -------------------------------------------------------------------------
	// Access. All O(1).
	// -------------------------------------------------------------------------

	/** True iff Handle refers to a live slot AND the slot's generation matches. */
	bool IsValid(FBuildingHandle Handle) const;

	/**
	 * Mutable record access. PRECONDITION: IsValid(Handle). Big-O: O(1).
	 * Used by O(1) "move" build hooks that patch one slot in place.
	 */
	FBuildingRecord& Get(FBuildingHandle Handle);
	const FBuildingRecord& Get(FBuildingHandle Handle) const;

	/** Safe access: returns nullptr if the handle is stale. O(1). */
	const FBuildingRecord* Find(FBuildingHandle Handle) const;
	FBuildingRecord* Find(FBuildingHandle Handle);

	/** Live building count (excludes freed slots). O(1). */
	int32 Num() const;

	/** Reserve capacity for N records to avoid rehash/realloc churn at gather. */
	void Reserve(int32 ExpectedCount);

	/** Drop all records and side-tables. Generations are preserved/advanced so
	 *  pre-clear handles stay invalid. O(N). */
	void Empty();

	// -------------------------------------------------------------------------
	// Typed side-table accessors. ExtraIndex on a record points here.
	// The owning agent guarantees Type<->table consistency. All O(1).
	// -------------------------------------------------------------------------

	/** Allocate a spline side-table entry; returns its ExtraIndex. */
	uint32 AddSplineExtra(const FSplineExtra& Extra);
	uint32 AddWireExtra(const FWireExtra& Extra);
	uint32 AddBeamExtra(const FBeamExtra& Extra);

	/** Resolve a record's ExtraIndex. PRECONDITION: the record's Type matches
	 *  the table queried and ExtraIndex != INDEX_NONE. O(1). */
	const FSplineExtra& GetSplineExtra(uint32 ExtraIndex) const;
	const FWireExtra& GetWireExtra(uint32 ExtraIndex) const;
	const FBeamExtra& GetBeamExtra(uint32 ExtraIndex) const;

	/** Free a side-table entry (called by Remove for the matching Type). O(1). */
	void RemoveSplineExtra(uint32 ExtraIndex);
	void RemoveWireExtra(uint32 ExtraIndex);
	void RemoveBeamExtra(uint32 ExtraIndex);

	// -------------------------------------------------------------------------
	// Iteration. Visits live records only. Order is unspecified (slot order),
	// which is fine: Z-order for drawing comes from the Z-band index, not here.
	// -------------------------------------------------------------------------

	/** Visit every live (handle, record) pair. Visitor: void(FBuildingHandle, const FBuildingRecord&). */
	template<typename FuncType>
	void ForEach(FuncType&& Visitor) const;

private:
	// -------------------------------------------------------------------------
	// Storage layout (owning agent: "store"). See SPEC 4.1.
	//
	// Records live in a TSparseArray<FBuildingRecord>: free-list slot reuse,
	// positions never shift, O(1) Add/RemoveAt, and the sparse-array allocation
	// flags ARE the liveness oracle. The handle Slot is the sparse-array index.
	//
	// Generations is a dense TArray<uint16> parallel to the sparse array's index
	// space (grown to GetMaxIndex). On free the slot's generation is bumped so a
	// recycled slot rejects any stale handle (ABA-safety). On (re)allocation of a
	// slot the CURRENT generation is the one stamped into the returned handle.
	//
	// The three typed side-tables are dense TArrays addressed by ExtraIndex, each
	// with its own free-list of recycled indices so a removed spline/wire/beam's
	// slot is reused without shifting (ExtraIndex on live records stays stable).
	// -------------------------------------------------------------------------

	/** Find a live record pointer for Handle (live slot + matching Gen), else null. */
	FORCEINLINE FBuildingRecord* ResolveChecked(FBuildingHandle Handle);
	FORCEINLINE const FBuildingRecord* ResolveChecked(FBuildingHandle Handle) const;

	/** Generic side-table allocation/free helpers (templated over column+free-list). */
	template<typename ElementType>
	static uint32 AddExtra(TArray<ElementType>& Column, TArray<uint32>& FreeList, const ElementType& Value);
	template<typename ElementType>
	static void RemoveExtra(TArray<ElementType>& Column, TArray<uint32>& FreeList, uint32 ExtraIndex);

	/** Record columns. Slot == sparse-array index; liveness == allocation flag. */
	TSparseArray<FBuildingRecord> Records;

	/** Per-slot generation, dense, parallel to Records' index space. */
	TArray<uint16> Generations;

	/** Typed side-tables addressed by ExtraIndex, each with its own free-list. */
	TArray<FSplineExtra> SplineExtras;
	TArray<uint32> SplineFreeList;

	TArray<FWireExtra> WireExtras;
	TArray<uint32> WireFreeList;

	TArray<FBeamExtra> BeamExtras;
	TArray<uint32> BeamFreeList;
};


// =============================================================================
// Inline template definitions (must live in the header so every translation
// unit can instantiate them). Non-template members are in the .cpp.
// =============================================================================

template<typename FuncType>
void FCartographBuildingStore::ForEach(FuncType&& Visitor) const
{
	// Iterate live slots only. TSparseArray's const iterator skips freed slots,
	// and GetIndex() yields the stable slot id we pair with its generation to
	// reconstruct the handle. Order is slot order (unspecified by contract);
	// Z-order for drawing comes from the Z-band index, never from here.
	// SPIKE(Q13): confirm TSparseArray::CreateConstIterator()'s iterator exposes
	// GetIndex() and skips freed slots on the CSS fork (stock UE5 does both). If
	// the iterator API differs, iterate [0, GetMaxIndex()) and gate on IsAllocated(i).
	for (auto It = Records.CreateConstIterator(); It; ++It)
	{
		const int32 Slot = It.GetIndex();
		const FBuildingHandle Handle((uint32)Slot, Generations[Slot]);
		Visitor(Handle, *It);
	}
}

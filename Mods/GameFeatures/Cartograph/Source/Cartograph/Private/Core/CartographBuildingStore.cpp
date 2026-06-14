#include "Core/CartographBuildingStore.h"

#include "CartographGameInstanceModule.h"  // CARTO_LOG / CARTO_LOG_ERROR / LogCartograph

// =============================================================================
// CartographBuildingStore.cpp - SoA / TSparseArray column store (SPEC 4.1).
// -----------------------------------------------------------------------------
// Implements FCartographBuildingStore: a generational-handle-keyed store with
// O(1) Add/Remove/Get, free-list slot reuse, ABA-safe per-slot generations, and
// three typed side-tables (spline/wire/beam) each with their own free-list.
//
// Invariants maintained here:
//   * Records (TSparseArray) owns the slim records; the allocation flags are the
//     liveness oracle. A handle's Slot is the sparse-array index, FOREVER stable
//     for the lifetime of that allocation.
//   * Generations is grown to cover every index the sparse array can hand out;
//     Generations[Slot] is the live generation. Add stamps it into the handle;
//     Remove bumps it AFTER freeing so any copy of the old handle is rejected.
//   * Side-table ExtraIndex values stay stable for live records: freed entries
//     are recycled via the per-table free-list, never by shifting the column.
//
// No sorting, no compaction, no redirector - that whole legacy triad is gone.
// =============================================================================


// -----------------------------------------------------------------------------
// Internal resolve helpers. A handle is live iff its slot is allocated AND the
// slot's current generation matches the handle's. This is the single gate every
// public accessor funnels through, so ABA-safety is enforced in exactly one place.
// -----------------------------------------------------------------------------
FORCEINLINE FBuildingRecord* FCartographBuildingStore::ResolveChecked(FBuildingHandle Handle)
{
	const uint32 Slot = Handle.Slot;
	// Reject the null handle and any slot the sparse array does not currently own.
	if (Slot == (uint32)INDEX_NONE || !Records.IsValidIndex((int32)Slot))
	{
		return nullptr;
	}
	// Generation guard: a recycled slot has a bumped generation, so a stale
	// handle (same Slot, old Gen) resolves to null instead of aliasing the new
	// building that now occupies the slot.
	if (Generations[Slot] != Handle.Gen)
	{
		return nullptr;
	}
	return &Records[(int32)Slot];
}

FORCEINLINE const FBuildingRecord* FCartographBuildingStore::ResolveChecked(FBuildingHandle Handle) const
{
	// Reuse the mutable path; const-correctness is restored on return.
	return const_cast<FCartographBuildingStore*>(this)->ResolveChecked(Handle);
}


// -----------------------------------------------------------------------------
// Mutation. O(1) amortized; the hot path from the build hooks.
// -----------------------------------------------------------------------------
FBuildingHandle FCartographBuildingStore::Add(const FBuildingRecord& Record)
{
	// TSparseArray::Add reuses a free-list slot when one exists (no shift), else
	// grows the dense storage. Either way the returned index is a stable slot.
	// SPIKE(Q13): verify TSparseArray<FBuildingRecord>::Add(const T&) returns the
	// allocated int32 index on the CSS fork (stock UE5 returns FSparseArrayAllocationInfo
	// from AddUninitialized but int32 from Add); if the fork's Add returns the
	// allocation-info struct instead, switch to `Records.Add(Record).Index` or
	// AddUninitialized + placement-construct.
	const int32 Slot = Records.Add(Record);

	// Keep the parallel generation array covering every index the sparse array
	// can return. A brand-new slot starts at generation 0; a recycled slot keeps
	// the generation that Remove bumped it to, which is exactly what makes a
	// previously-issued handle for that slot stale.
	if (Slot >= Generations.Num())
	{
		// New high-water slot. Grow with zero-initialized generations; AddZeroed
		// covers the gap in case the sparse array skipped ahead (it does not, but
		// this keeps the array length-correct regardless of allocation strategy).
		Generations.AddZeroed(Slot + 1 - Generations.Num());
	}

	return FBuildingHandle((uint32)Slot, Generations[Slot]);
}

bool FCartographBuildingStore::Remove(FBuildingHandle Handle)
{
	FBuildingRecord* Record = ResolveChecked(Handle);
	if (!Record)
	{
		// Stale or null handle: idempotent no-op (a double-remove or a delta that
		// races a prior remove must not corrupt the store).
		return false;
	}

	const uint32 Slot = Handle.Slot;

	// Free the matching typed side-table entry, if any, so its slot is recycled.
	// The record's Type selects exactly one table; a belt never touches the
	// rectangle path. ExtraIndex == INDEX_NONE means "no extra" (Icon/Rectangle).
	const uint32 ExtraIndex = Record->ExtraIndex;
	if (ExtraIndex != (uint32)INDEX_NONE)
	{
		switch (Record->Type)
		{
		case EBuildingDrawType::Spline:
			RemoveSplineExtra(ExtraIndex);
			break;
		case EBuildingDrawType::Wire:
			RemoveWireExtra(ExtraIndex);
			break;
		case EBuildingDrawType::Beam:
			RemoveBeamExtra(ExtraIndex);
			break;
		default:
			// Icon/Rectangle/Invalid carry no side-table entry. A non-NONE
			// ExtraIndex on such a type is a caller invariant violation; log it
			// but do not touch any table (we cannot know which one it indexes).
			CARTO_LOG_ERROR("Remove: record type %u has ExtraIndex %u but no side-table",
				(uint32)Record->Type, ExtraIndex);
			break;
		}
	}

	// Free the record slot (O(1); pushes onto the sparse array's free-list).
	Records.RemoveAt((int32)Slot);

	// Bump the slot's generation AFTER freeing. uint16 wrap is acceptable: an old
	// handle only re-aliases after 65536 reuses of the same slot, which is
	// astronomically beyond any single network delta / UI pick lifetime.
	++Generations[Slot];

	return true;
}


// -----------------------------------------------------------------------------
// Access. All O(1).
// -----------------------------------------------------------------------------
bool FCartographBuildingStore::IsValid(FBuildingHandle Handle) const
{
	return ResolveChecked(Handle) != nullptr;
}

FBuildingRecord& FCartographBuildingStore::Get(FBuildingHandle Handle)
{
	// PRECONDITION: IsValid(Handle). checkSlow keeps the hot path branch-free in
	// shipping while still catching misuse in dev/test builds.
	FBuildingRecord* Record = ResolveChecked(Handle);
	checkSlow(Record != nullptr);
	return *Record;
}

const FBuildingRecord& FCartographBuildingStore::Get(FBuildingHandle Handle) const
{
	const FBuildingRecord* Record = ResolveChecked(Handle);
	checkSlow(Record != nullptr);
	return *Record;
}

const FBuildingRecord* FCartographBuildingStore::Find(FBuildingHandle Handle) const
{
	return ResolveChecked(Handle);
}

FBuildingRecord* FCartographBuildingStore::Find(FBuildingHandle Handle)
{
	return ResolveChecked(Handle);
}

int32 FCartographBuildingStore::Num() const
{
	// Live count excludes freed slots: TSparseArray::Num() reports allocated
	// elements only (GetMaxIndex would include the free-list holes).
	return Records.Num();
}

void FCartographBuildingStore::Reserve(int32 ExpectedCount)
{
	if (ExpectedCount <= 0)
	{
		return;
	}
	// Pre-size the record storage AND the parallel generation array so a gather
	// of N buildings does not churn reallocations. Reserving Generations to the
	// same count avoids per-Add growth on the cold load path.
	// SPIKE(Q13): confirm TSparseArray exposes Reserve(int32) on the CSS fork
	// (stock UE5 does). If not, this can be dropped (correctness is unaffected;
	// only the cold-load allocation churn the SPEC §9 Phase-1 metric cares about).
	Records.Reserve(ExpectedCount);
	Generations.Reserve(ExpectedCount);
}

void FCartographBuildingStore::Empty()
{
	// Advance the generation of every currently-live slot so any handle captured
	// before the clear stays invalid afterwards (a recycled slot would otherwise
	// re-issue Gen 0 and could alias a pre-clear handle). We must do this BEFORE
	// emptying Records, while the allocation flags still mark the live slots.
	for (auto It = Records.CreateConstIterator(); It; ++It)
	{
		++Generations[It.GetIndex()];
	}

	// Drop all records and side-table contents. Generations is intentionally NOT
	// emptied: preserving (and having just advanced) per-slot generations is what
	// keeps pre-clear handles invalid after slots are reused.
	Records.Empty();

	SplineExtras.Empty();
	SplineFreeList.Empty();
	WireExtras.Empty();
	WireFreeList.Empty();
	BeamExtras.Empty();
	BeamFreeList.Empty();
}


// -----------------------------------------------------------------------------
// Typed side-tables. Each is a dense column with a free-list of recycled
// indices. AddExtra reuses a freed index when available (so live ExtraIndex
// values never shift), else appends. RemoveExtra pushes the index back.
// -----------------------------------------------------------------------------
template<typename ElementType>
uint32 FCartographBuildingStore::AddExtra(TArray<ElementType>& Column, TArray<uint32>& FreeList, const ElementType& Value)
{
	if (FreeList.Num() > 0)
	{
		// Reuse the most-recently-freed index (LIFO is cache-friendliest).
		const uint32 ReusedIndex = FreeList.Pop(EAllowShrinking::No);
		Column[(int32)ReusedIndex] = Value;
		return ReusedIndex;
	}

	const int32 NewIndex = Column.Add(Value);
	return (uint32)NewIndex;
}

template<typename ElementType>
void FCartographBuildingStore::RemoveExtra(TArray<ElementType>& Column, TArray<uint32>& FreeList, uint32 ExtraIndex)
{
	if (ExtraIndex == (uint32)INDEX_NONE || !Column.IsValidIndex((int32)ExtraIndex))
	{
		CARTO_LOG_ERROR("RemoveExtra: invalid ExtraIndex %u (column size %d)", ExtraIndex, Column.Num());
		return;
	}

	// Reset the freed element to default so a stale read of a recycled-but-not-yet-
	// reassigned slot yields empty geometry rather than the previous building's data.
	Column[(int32)ExtraIndex] = ElementType{};
	FreeList.Push(ExtraIndex);
}

uint32 FCartographBuildingStore::AddSplineExtra(const FSplineExtra& Extra)
{
	return AddExtra(SplineExtras, SplineFreeList, Extra);
}

uint32 FCartographBuildingStore::AddWireExtra(const FWireExtra& Extra)
{
	return AddExtra(WireExtras, WireFreeList, Extra);
}

uint32 FCartographBuildingStore::AddBeamExtra(const FBeamExtra& Extra)
{
	return AddExtra(BeamExtras, BeamFreeList, Extra);
}

const FSplineExtra& FCartographBuildingStore::GetSplineExtra(uint32 ExtraIndex) const
{
	// PRECONDITION: caller guarantees the record's Type == Spline and a live index.
	checkSlow(SplineExtras.IsValidIndex((int32)ExtraIndex));
	return SplineExtras[(int32)ExtraIndex];
}

const FWireExtra& FCartographBuildingStore::GetWireExtra(uint32 ExtraIndex) const
{
	checkSlow(WireExtras.IsValidIndex((int32)ExtraIndex));
	return WireExtras[(int32)ExtraIndex];
}

const FBeamExtra& FCartographBuildingStore::GetBeamExtra(uint32 ExtraIndex) const
{
	checkSlow(BeamExtras.IsValidIndex((int32)ExtraIndex));
	return BeamExtras[(int32)ExtraIndex];
}

void FCartographBuildingStore::RemoveSplineExtra(uint32 ExtraIndex)
{
	RemoveExtra(SplineExtras, SplineFreeList, ExtraIndex);
}

void FCartographBuildingStore::RemoveWireExtra(uint32 ExtraIndex)
{
	RemoveExtra(WireExtras, WireFreeList, ExtraIndex);
}

void FCartographBuildingStore::RemoveBeamExtra(uint32 ExtraIndex)
{
	RemoveExtra(BeamExtras, BeamFreeList, ExtraIndex);
}

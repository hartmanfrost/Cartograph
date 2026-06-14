#include "Core/CartographSpatialGrid.h"

#include "CartographConfig.h"
#include "CartographGameInstanceModule.h"  // CARTO_LOG_* macros (LogCartograph)

// =============================================================================
// CartographSpatialGrid.cpp - uniform fixed-cell spatial hash grid (SPEC 4.1).
// -----------------------------------------------------------------------------
// A flat row-major TArray of GRID_CELL_COUNT cells over the anisotropic world
// bounds. Each cell is a small inline-allocated TArray<FBuildingHandle>.
//
//   Insert  = O(cells-covered) push by handle into each covered cell.
//   Remove  = O(cells-covered) cell lookups, O(1) RemoveAtSwap per cell.
//   Query   = O(cells-in-rect + hits), cell-granular (false positives OK).
//
// There is NO redirector, NO equal-Z == scan, NO array reindex on mutation:
// the handle is the stable identity, and a covered cell is found in O(1) from
// the box corners. This is the wholesale replacement for the legacy
// TQuadTree<int32> + BuildingDataIndexRedirector triad (root cause 1).
// =============================================================================


void FCartographSpatialGrid::Initialize()
{
	// GRID_CELL_COUNT is a constexpr derived from the anisotropic bounds with
	// per-axis ceil counts (CartographConfig). SetNum value-initializes each
	// FCellArray to empty; subsequent Insert grows cells lazily.
	if (GRID_CELL_COUNT <= 0)
	{
		// Should be impossible given constexpr positive bounds; guard so a
		// future bounds edit that goes degenerate fails loud, not silent-empty.
		CARTO_LOG_ERROR("SpatialGrid::Initialize: non-positive GRID_CELL_COUNT (%d); grid left empty", GRID_CELL_COUNT);
		Cells.Reset();
		return;
	}

	Cells.Empty(GRID_CELL_COUNT);
	Cells.SetNum(GRID_CELL_COUNT);

	CARTO_LOG("SpatialGrid::Initialize: %d cells (%dx%d), cell %.0f cm",
		GRID_CELL_COUNT, GRID_CELLS_X, GRID_CELLS_Y, GRID_CELL_SIZE_CM);
}


void FCartographSpatialGrid::Insert(FBuildingHandle Handle, const FBox2f& WorldBox)
{
	if (Cells.Num() == 0)
	{
		CARTO_LOG_ERROR("SpatialGrid::Insert before Initialize(); dropping handle (Slot=%u)", Handle.Slot);
		return;
	}

	FIntPoint MinCell, MaxCell;
	CellRangeFromBox(WorldBox, MinCell, MaxCell);

	// Push the handle into every cell the box's inclusive cell range covers.
	// Single-cell buildings (the common case) touch exactly one cell; large
	// foundations / long belts touch the full ragged range. The caller is
	// responsible for passing the SAME box to Remove (contract), so we never
	// store the coverage on the record here.
	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		const int32 RowBase = CellY * GRID_CELLS_X;
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			Cells[RowBase + CellX].Add(Handle);
		}
	}
}


void FCartographSpatialGrid::Remove(FBuildingHandle Handle, const FBox2f& WorldBox)
{
	if (Cells.Num() == 0)
	{
		return;
	}

	FIntPoint MinCell, MaxCell;
	CellRangeFromBox(WorldBox, MinCell, MaxCell);

	// Visit exactly the cells Insert touched (same box => same coverage) and
	// swap-remove the handle from each. RemoveAtSwap is O(1): it overwrites the
	// slot with the cell's last element and shrinks - NO order preservation
	// needed (the cell is an unordered bucket), NO redirector walk, NO reindex.
	//
	// A handle appears at most once per cell (Insert pushes once per covered
	// cell), so we can stop scanning a cell after the first match. We still do
	// a linear find WITHIN the cell because cells are tiny (a handful of
	// handles); a per-cell TMap would cost more than it saves at this density.
	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		const int32 RowBase = CellY * GRID_CELLS_X;
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			FCellArray& Cell = Cells[RowBase + CellX];
			const int32 Found = Cell.IndexOfByKey(Handle);
			if (Found != INDEX_NONE)
			{
				// Swap-with-last + pop: O(1), unordered bucket semantics. Explicit
				// (Index, Count, EAllowShrinking) overload - verified on the fork
				// via the identical IndexOfByKey+RemoveAt pattern in
				// FGSignSubsystem.h:161-164 (RemoveAtSwap shares the signature).
				Cell.RemoveAtSwap(Found, 1, EAllowShrinking::No);
			}
		}
	}
}


void FCartographSpatialGrid::QueryRect(const FBox2f& WorldQueryBox, TArray<FBuildingHandle>& OutHandles) const
{
	// Delegate to the inline visitor form so the cell-iteration logic lives in
	// exactly one place. OutHandles is appended to, not reset (contract). May
	// contain duplicates (multi-cell buildings) and false positives
	// (cell-granular) - the caller refines against the record's exact VisualBox.
	QueryRect(WorldQueryBox, [&OutHandles](FBuildingHandle Handle)
	{
		OutHandles.Add(Handle);
	});
}


int32 FCartographSpatialGrid::NumEntries() const
{
	// Counts multi-cell duplicates (a building spanning K cells contributes K).
	// This is a debug/telemetry accessor, not a hot path; an O(cells) sum is
	// fine since cell count is fixed and small relative to building count.
	int32 Total = 0;
	for (const FCellArray& Cell : Cells)
	{
		Total += Cell.Num();
	}
	return Total;
}


void FCartographSpatialGrid::Reset()
{
	// Clear every cell but KEEP the per-cell allocations and the flat array
	// allocation - a re-gather/rebuild refills the same buckets without
	// reallocating GRID_CELL_COUNT arrays. O(cells).
	for (FCellArray& Cell : Cells)
	{
		Cell.Reset();
	}
}

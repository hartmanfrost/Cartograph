#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"

// =============================================================================
// CartographSpatialGrid.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "grid" agent (CartographSpatialGrid.cpp).
// -----------------------------------------------------------------------------
// Uniform fixed-cell spatial hash grid keyed by FBuildingHandle (SPEC 4.1).
// Replaces the legacy TQuadTree<int32> + BuildingDataIndexRedirector triad.
//
// Geometry comes from CartographConfig: GRID_CELLS_X * GRID_CELLS_Y cells over
// the anisotropic world bounds, per-axis ceil cell counts, ragged edge cells
// tolerated. A flat TArray of cells, each a small TArray<FBuildingHandle>.
//
// Multi-cell support: a building whose footprint/spline spans several cells is
// inserted into every covered cell. The caller passes the SAME FBox2f to
// Remove that it passed to Insert so the grid removes from exactly those cells
// (no need to re-derive coverage). Remove from a cell is O(1) swap-remove by
// handle - NO equal-Z == scan, NO redirector walk.
//
// The grid stores handles only; it never owns geometry. Coordinates are WORLD
// centimeters (XY). The caller converts as needed.
// =============================================================================
class CARTOGRAPH_API FCartographSpatialGrid
{
public:
	FCartographSpatialGrid() = default;
	~FCartographSpatialGrid() = default;

	FCartographSpatialGrid(const FCartographSpatialGrid&) = delete;
	FCartographSpatialGrid& operator=(const FCartographSpatialGrid&) = delete;

	/** Allocate the flat cell array (GRID_CELL_COUNT cells). Call once at init. */
	void Initialize();

	/**
	 * Insert Handle into every cell its world-space box covers.
	 * @param WorldBox  building bounds in WORLD centimeters (XY).
	 * Big-O: O(cells-covered). Push by handle into each covered cell.
	 */
	void Insert(FBuildingHandle Handle, const FBox2f& WorldBox);

	/**
	 * Remove Handle from every cell WorldBox covers. Pass the SAME box used at
	 * Insert (the caller owns the box, e.g. from the record's draw geometry).
	 * Big-O: O(cells-covered) cell lookups, O(1) swap-remove per cell.
	 */
	void Remove(FBuildingHandle Handle, const FBox2f& WorldBox);

	/**
	 * Append every handle whose cell intersects WorldQueryBox to OutHandles.
	 * May contain duplicates for multi-cell buildings AND false positives
	 * (cell-granular, not box-exact) - the caller refines against the record's
	 * exact VisualBox. OutHandles is appended to, not reset.
	 * Big-O: O(cells-in-rect + hits).
	 */
	void QueryRect(const FBox2f& WorldQueryBox, TArray<FBuildingHandle>& OutHandles) const;

	/**
	 * Visitor form to avoid an allocation. Visitor: void(FBuildingHandle).
	 * Same duplicate/false-positive semantics as QueryRect.
	 */
	template<typename FuncType>
	void QueryRect(const FBox2f& WorldQueryBox, FuncType&& Visitor) const;

	/** Total handle entries across all cells (counts multi-cell duplicates). */
	int32 NumEntries() const;

	/** Clear all cells; keeps the allocation. O(cells). */
	void Reset();

private:
	// A flat row-major array of GRID_CELL_COUNT cells, indexed by
	// CellY * GRID_CELLS_X + CellX. Each cell is a small inline-allocated array
	// of handles. The grid stores handles ONLY (no geometry, no boxes): the
	// legacy quadtree's 40 B/element FBox2D is gone entirely (SPEC 4.1).
	//
	// Box coverage is the inclusive cell range [min-cell, max-cell] on each
	// axis, derived from the box corners via CartographCoords::GridCellFromWorld
	// (which already clamps to [0, GRID_CELLS_*-1] so ragged edge boxes and
	// out-of-bounds coordinates fold onto the border cells).
	//
	// Cells hold a TInlineAllocator slack so the common case (a single-cell
	// building, one handle per cell) needs no heap allocation, while large
	// foundations / long belts that span many cells spill to the heap.
	using FCellArray = TArray<FBuildingHandle, TInlineAllocator<4>>;

	/** Flat cell store; empty until Initialize(). */
	TArray<FCellArray> Cells;

	/**
	 * Inclusive cell-coordinate range a world-space box covers. The returned
	 * Min/Max are already clamped into the valid grid by GridCellFromWorld so
	 * callers can iterate [Min.X..Max.X] x [Min.Y..Max.Y] unconditionally.
	 */
	FORCEINLINE void CellRangeFromBox(const FBox2f& WorldBox, FIntPoint& OutMin, FIntPoint& OutMax) const
	{
		OutMin = CartographCoords::GridCellFromWorld((double)WorldBox.Min.X, (double)WorldBox.Min.Y);
		OutMax = CartographCoords::GridCellFromWorld((double)WorldBox.Max.X, (double)WorldBox.Max.Y);
		// Defensive: a degenerate/inverted box (Min > Max) would otherwise yield
		// an empty iteration; GridCellFromWorld clamps but does not reorder.
		if (OutMin.X > OutMax.X) { Swap(OutMin.X, OutMax.X); }
		if (OutMin.Y > OutMax.Y) { Swap(OutMin.Y, OutMax.Y); }
	}

	/** Flat cell index from clamped cell coordinates. Row-major. */
	FORCEINLINE int32 CellIndex(int32 CellX, int32 CellY) const
	{
		return CellY * GRID_CELLS_X + CellX;
	}
};


// -----------------------------------------------------------------------------
// QueryRect (visitor form) - header-inline so callers can pass a lambda without
// a TArray allocation. Iterates the inclusive cell range the query box covers
// and visits every handle in each covered cell. Duplicates (multi-cell
// buildings) and false positives (cell-granular) are intentional; the caller
// refines against the record's exact VisualBox (SPEC 4.1).
// -----------------------------------------------------------------------------
template<typename FuncType>
void FCartographSpatialGrid::QueryRect(const FBox2f& WorldQueryBox, FuncType&& Visitor) const
{
	if (Cells.Num() == 0)
	{
		return;
	}

	FIntPoint MinCell, MaxCell;
	CellRangeFromBox(WorldQueryBox, MinCell, MaxCell);

	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		const int32 RowBase = CellY * GRID_CELLS_X;
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			const FCellArray& Cell = Cells[RowBase + CellX];
			for (const FBuildingHandle& Handle : Cell)
			{
				Visitor(Handle);
			}
		}
	}
}

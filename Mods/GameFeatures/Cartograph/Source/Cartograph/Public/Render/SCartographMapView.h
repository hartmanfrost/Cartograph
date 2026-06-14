#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/RenderingCommon.h"  // FSlateVertex, SlateIndex (instance buffer)
#include "Core/CartographTypes.h"

struct FSlateBrush;

// =============================================================================
// SCartographMapView.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "slate" agent (SCartographMapView.cpp). PHASE 3 / B.
// CVar-GATED: only used when r.Cartograph.RenderBackend == 1 (SlateInstanced).
// The TiledCanvas compositor is the always-available fallback (SPEC 4.2, Q4).
// -----------------------------------------------------------------------------
// Custom Slate leaf widget whose OnPaint emits a per-instance vertex buffer via
// FSlateDrawElement::MakeCustomVerts - one instanced draw batching all visible
// buildings (a unit quad/line per building). ~10x cheaper on the render thread
// than FCanvas-to-RT, deletes the persistent VRAM atlas, and removes the
// process-wide FCanvas::GetBatchedElements hook by construction (SPEC 4.2).
//
// Z-order: MakeCustomVerts is not depth-sorted across instances, so draw order
// is baked into INSTANCE order via the Z-band index (paint back-to-front).
// Zoom-LOD: instance-count culling to the viewport clip rect (no texture mips).
//
// SPIKE(Q4): MakeCustomVerts / SLeafWidget symbol export and UMG compositing are
// unverified on the CSS Slate build - validate the standalone spike (100k
// custom-vert quads into the map panel) before defaulting to this backend.
// =============================================================================
class FCartographBuildingStore;
class FCartographSpatialGrid;
class FCartographZBandIndex;
class FCartographClassDrawTable;


class CARTOGRAPH_API SCartographMapView : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCartographMapView) {}
		// World->screen and viewport state are pushed via the setters below
		// rather than slate args so UMG can drive pan/zoom imperatively.
	SLATE_END_ARGS()

	/** Slate construct. Binds the data spine (non-owning; the owning subsystem
	 *  outlives the widget). */
	void Construct(
		const FArguments& InArgs,
		FCartographBuildingStore* InStore,
		FCartographSpatialGrid* InGrid,
		FCartographZBandIndex* InZBands,
		FCartographClassDrawTable* InClassTable);

	// -------------------------------------------------------------------------
	// SWidget interface (implemented by the slate agent).
	// -------------------------------------------------------------------------

	/**
	 * Build and submit the per-instance buffer for the current viewport AoI.
	 * Walks the Z-band index low->high (back-to-front) intersected with the
	 * grid query for ViewportWorldRect, computes each building's geometry via
	 * FCartographClassDrawTable::ComputeDrawGeometry, appends to a
	 * FSlateVertex / index buffer, and emits one MakeCustomVerts draw element.
	 * Big-O: O(buildings in AoI).
	 */
	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	/** Leaf widget: no desired size of its own (fills its UMG slot). */
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

	// -------------------------------------------------------------------------
	// Viewport / filter control (driven by UMG pan/zoom). Each setter
	// Invalidates the widget so the next paint rebuilds the instance buffer.
	// -------------------------------------------------------------------------

	/** Set the visible WORLD rect (cm, XY) the paint culls/queries against. */
	void SetViewportWorldRect(const FBox2D& WorldRect);

	/** Set the Z height-filter range (world cm). */
	void SetZFilter(float MinZ, float MaxZ);

	/** Toggle building visibility (legacy DoShowBuildings). */
	void SetShowBuildings(bool bShow);

	/**
	 * Set the resource (texture/material) the custom-verts draw samples. The
	 * Phase-B instanced path batches every building into ONE MakeCustomVerts
	 * element, so all instances must share a single resource handle (icons are
	 * resolved to UVs inside this shared atlas/material; the spike validates the
	 * exact resource form). Passing nullptr leaves the widget inert (no draw).
	 * SPIKE(Q4): the resource form (FSlateBrush vs a custom-verts material handle)
	 * that composites correctly through FG's UMG/HUD is unverified on CSS Slate.
	 */
	void SetCustomVertsBrush(const FSlateBrush* InBrush);

private:
	// -------------------------------------------------------------------------
	// Data spine (non-owning; the owning subsystem outlives this widget).
	// -------------------------------------------------------------------------
	FCartographBuildingStore* Store = nullptr;
	FCartographSpatialGrid* Grid = nullptr;
	FCartographZBandIndex* ZBands = nullptr;
	FCartographClassDrawTable* ClassTable = nullptr;

	// -------------------------------------------------------------------------
	// Viewport / filter state (driven imperatively by UMG; each setter
	// Invalidates so the next paint rebuilds the instance buffer).
	// -------------------------------------------------------------------------

	/** Visible WORLD rect (cm, XY) the paint culls/queries against. */
	FBox2D ViewportWorldRect = FBox2D(ForceInit);

	/** Z height-filter range (world cm). Defaults to the full binning span. */
	float ZFilterMin = (float)Z_BAND_MIN_CM;
	float ZFilterMax = (float)Z_BAND_MAX_CM;

	/** Whether buildings draw at all (legacy DoShowBuildings). */
	bool bShowBuildings = true;

	/** Shared resource the single MakeCustomVerts batch samples (SPIKE Q4). */
	const FSlateBrush* CustomVertsBrush = nullptr;

	// -------------------------------------------------------------------------
	// Paint helpers (const; OnPaint is const and the instance buffer is rebuilt
	// each paint into locals, never cached on the widget).
	// -------------------------------------------------------------------------

	/**
	 * Map an atlas-pixel-space position (RENDER_TEXTURE_SIZE space, as produced
	 * by ComputeDrawGeometry/CartographCoords::WorldToScreen) into the widget's
	 * local Slate space for the current ViewportWorldRect + AllottedGeometry.
	 * This is the pan/zoom transform; LOD is expressed as which instances survive
	 * the AoI cull, not as a texture mip.
	 */
	FVector2f AtlasPixelToLocal(const FVector2f& AtlasPixel, const FVector2f& AtlasViewMin, const FVector2f& LocalScale) const;

	/**
	 * Dispatch one computed building geometry into the instance buffer by draw
	 * type (Icon/Rectangle -> quad [+ outline strokes]; Spline -> polyline;
	 * Wire/Beam -> thick line). Appends to the shared per-paint vert/index arrays.
	 * RenderTransform is the widget's accumulated render transform (local Slate
	 * space -> window space), baked into each emitted FSlateVertex.
	 */
	void AppendInstance(
		const FDrawGeometry& Geo,
		const FSlateRenderTransform& RenderTransform,
		const FVector2f& AtlasViewMin,
		const FVector2f& LocalScale,
		TArray<FSlateVertex>& OutVerts,
		TArray<SlateIndex>& OutIndices) const;

	/** Append one screen-space (atlas-pixel) quad as 2 triangles (6 indices).
	 *  Corners are CW from top-left (the FDrawGeometry::Corners convention). */
	void AppendQuad(
		const FVector2f Corners[4],
		const FSlateRenderTransform& RenderTransform,
		const FVector2f& AtlasViewMin,
		const FVector2f& LocalScale,
		const FColor& VertexColor,
		TArray<FSlateVertex>& OutVerts,
		TArray<SlateIndex>& OutIndices) const;

	/** Append a thick line segment as a quad (2 triangles) so MakeCustomVerts can
	 *  batch strokes alongside fills in the same instanced draw. */
	void AppendThickLine(
		const FVector2f& A,
		const FVector2f& B,
		float ThicknessPx,
		const FSlateRenderTransform& RenderTransform,
		const FVector2f& AtlasViewMin,
		const FVector2f& LocalScale,
		const FColor& VertexColor,
		TArray<FSlateVertex>& OutVerts,
		TArray<SlateIndex>& OutIndices) const;
};

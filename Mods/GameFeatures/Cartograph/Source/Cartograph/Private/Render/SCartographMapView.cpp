#include "Render/SCartographMapView.h"

#include "CartographConfig.h"
#include "Core/CartographBuildingStore.h"
#include "Core/CartographSpatialGrid.h"
#include "Core/CartographZBandIndex.h"
#include "Core/CartographClassDrawTable.h"

#include "Rendering/DrawElements.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateRenderTransform.h"
#include "Styling/SlateBrush.h"
#include "Framework/Application/SlateApplication.h"  // FSlateApplication::Get().GetRenderer()->GetResourceHandle

// Logging: LogCartograph is declared in CartographConfig.h (included above);
// only the one log macro this file needs is defined below, in the repo's
// CARTO_LOG style. We intentionally avoid the heavy CartographGameInstanceModule.h.

#ifndef CARTO_LOG_VERY_VERBOSE
#define CARTO_LOG_VERY_VERBOSE(format, ...) if constexpr (false) UE_LOG(LogCartograph, Display, TEXT(format) __VA_OPT__(, __VA_ARGS__))
#endif

// =============================================================================
// SCartographMapView.cpp - PHASE 3 / B (CVar-gated SlateInstanced backend).
// -----------------------------------------------------------------------------
// Custom Slate leaf widget. OnPaint walks the Z-band index low->high
// (back-to-front), intersects with the spatial-grid query for the current
// viewport AoI, computes each building's screen geometry via the shared pure
// FCartographClassDrawTable::ComputeDrawGeometry, appends a per-instance vertex
// buffer (unit quad / thick-line quad per primitive), and emits exactly ONE
// FSlateDrawElement::MakeCustomVerts draw element for the whole viewport.
//
// Why this backend exists (SPEC 4.2 Phase B):
//  - one instanced draw batches all visible buildings (~10x cheaper on the
//    render thread than FCanvas-to-RT per Froyok's benchmark),
//  - it DELETES the persistent VRAM atlas (the panel composites via Slate's own
//    swap-chain pass; nothing persistent is allocated here),
//  - it removes the process-wide FCanvas::GetBatchedElements hook by
//    construction (this path never touches FCanvas).
//
// Z-order: MakeCustomVerts is NOT depth-sorted across instances, so paint order
// IS the Z-band order (low band first == back-to-front). Zoom-LOD: instances
// outside the viewport clip rect are culled by the grid AoI query (no mips).
//
// This backend is INERT unless r.Cartograph.RenderBackend == 1 (SlateInstanced):
// OnPaint early-outs to the unchanged base LayerId so the TiledCanvas compositor
// remains the always-available fallback (SPEC 4.2, Q4).
//
// SPIKE(Q4): FSlateDrawElement::MakeCustomVerts and SLeafWidget symbol-export +
// UMG/HUD compositing are UNVERIFIED on the CSS Slate build. Validate the
// standalone spike (100k custom-vert quads into the map panel) before defaulting
// to this backend; if it fails, Phase A (TiledCanvas) remains the permanent
// floor.
// =============================================================================


// -----------------------------------------------------------------------------
// Construct
// -----------------------------------------------------------------------------
void SCartographMapView::Construct(
	const FArguments& InArgs,
	FCartographBuildingStore* InStore,
	FCartographSpatialGrid* InGrid,
	FCartographZBandIndex* InZBands,
	FCartographClassDrawTable* InClassTable)
{
	Store = InStore;
	Grid = InGrid;
	ZBands = InZBands;
	ClassTable = InClassTable;

	// Leaf widget paints its own content; never hit-test the verts (picking goes
	// through the spatial grid, not Slate).
	SetCanTick(false);
}


// -----------------------------------------------------------------------------
// Viewport / filter control. Each setter Invalidates so the next frame's OnPaint
// rebuilds the instance buffer (the buffer is never cached on the widget; the
// invalidation just re-triggers a paint when the UMG panel is otherwise static).
// -----------------------------------------------------------------------------
void SCartographMapView::SetViewportWorldRect(const FBox2D& WorldRect)
{
	ViewportWorldRect = WorldRect;
	Invalidate(EInvalidateWidgetReason::Paint);
}


void SCartographMapView::SetZFilter(float MinZ, float MaxZ)
{
	ZFilterMin = FMath::Min(MinZ, MaxZ);
	ZFilterMax = FMath::Max(MinZ, MaxZ);
	Invalidate(EInvalidateWidgetReason::Paint);
}


void SCartographMapView::SetShowBuildings(bool bShow)
{
	if (bShowBuildings != bShow)
	{
		bShowBuildings = bShow;
		Invalidate(EInvalidateWidgetReason::Paint);
	}
}


void SCartographMapView::SetCustomVertsBrush(const FSlateBrush* InBrush)
{
	CustomVertsBrush = InBrush;
	Invalidate(EInvalidateWidgetReason::Paint);
}


// -----------------------------------------------------------------------------
// ComputeDesiredSize - leaf widget fills its UMG slot, no intrinsic size.
// -----------------------------------------------------------------------------
FVector2D SCartographMapView::ComputeDesiredSize(float /*LayoutScaleMultiplier*/) const
{
	return FVector2D::ZeroVector;
}


// -----------------------------------------------------------------------------
// Atlas-pixel -> widget-local mapping (the pan/zoom transform).
//
// ComputeDrawGeometry produces positions in RENDER_TEXTURE_SIZE atlas-pixel
// space (CartographCoords::WorldToScreen). The viewport world rect maps to an
// atlas-pixel rect [AtlasViewMin, AtlasViewMax]; we scale that rect to fill the
// widget's local geometry. LocalScale is local-px per atlas-px on each axis.
// -----------------------------------------------------------------------------
FVector2f SCartographMapView::AtlasPixelToLocal(
	const FVector2f& AtlasPixel,
	const FVector2f& AtlasViewMin,
	const FVector2f& LocalScale) const
{
	return FVector2f(
		(AtlasPixel.X - AtlasViewMin.X) * LocalScale.X,
		(AtlasPixel.Y - AtlasViewMin.Y) * LocalScale.Y);
}


// -----------------------------------------------------------------------------
// AppendQuad - one screen-space (atlas-pixel) quad as 2 triangles.
// Corners are CW from top-left (the FDrawGeometry::Corners convention). Each
// corner is mapped atlas-pixel -> widget-local (pan/zoom), then the widget's
// accumulated render transform (local -> window space) is baked into the vertex
// by FSlateVertex::Make so the batch lands at the panel's on-screen position.
// -----------------------------------------------------------------------------
void SCartographMapView::AppendQuad(
	const FVector2f Corners[4],
	const FSlateRenderTransform& RenderTransform,
	const FVector2f& AtlasViewMin,
	const FVector2f& LocalScale,
	const FColor& VertexColor,
	TArray<FSlateVertex>& OutVerts,
	TArray<SlateIndex>& OutIndices) const
{
	const SlateIndex Base = (SlateIndex)OutVerts.Num();

	// Unit-quad UVs; the shared resource resolves the actual texel. Solid-color
	// primitives (rectangle fills, strokes) ignore the texel (white sample);
	// icons need per-icon sub-rect UVs from a shared atlas/material - SPIKE Q4 for
	// the exact resource form + per-icon UV packing.
	static const FVector2f QuadUV[4] = {
		FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f)
	};

	for (int32 CornerIdx = 0; CornerIdx < 4; ++CornerIdx)
	{
		const FVector2f Local = AtlasPixelToLocal(Corners[CornerIdx], AtlasViewMin, LocalScale);
		// SPIKE(Q4): FSlateVertex::Make signature (FSlateRenderTransform, pos, uv,
		// color) vs the older overloads is unverified on the CSS Slate build.
		OutVerts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(
			RenderTransform,
			Local,
			QuadUV[CornerIdx],
			VertexColor));
	}

	// Two triangles: (0,1,2) and (0,2,3).
	OutIndices.Add(Base + 0);
	OutIndices.Add(Base + 1);
	OutIndices.Add(Base + 2);
	OutIndices.Add(Base + 0);
	OutIndices.Add(Base + 2);
	OutIndices.Add(Base + 3);
}


// -----------------------------------------------------------------------------
// AppendThickLine - a thick line segment as a quad (2 triangles). Thickness is
// in atlas pixels (matches legacy FCanvasLineItem::LineThickness, which is in
// the same atlas-pixel space). We expand by half-thickness along the segment
// normal, then map both expanded corners to local space so strokes batch into
// the same instanced draw as fills (no separate FCanvasLineItem path).
// -----------------------------------------------------------------------------
void SCartographMapView::AppendThickLine(
	const FVector2f& A,
	const FVector2f& B,
	float ThicknessPx,
	const FSlateRenderTransform& RenderTransform,
	const FVector2f& AtlasViewMin,
	const FVector2f& LocalScale,
	const FColor& VertexColor,
	TArray<FSlateVertex>& OutVerts,
	TArray<SlateIndex>& OutIndices) const
{
	const FVector2f Dir = B - A;
	const float Len = Dir.Size();
	if (Len <= KINDA_SMALL_NUMBER)
	{
		return;  // degenerate segment - nothing to draw (matches legacy "don't bother")
	}

	const FVector2f Unit = Dir / Len;
	const FVector2f Normal(-Unit.Y, Unit.X);
	const float Half = FMath::Max(ThicknessPx, 1.f) * 0.5f;
	const FVector2f Offset = Normal * Half;

	// Quad corners in atlas-pixel space, CW: A+off, B+off, B-off, A-off.
	const FVector2f Corners[4] = {
		A + Offset,
		B + Offset,
		B - Offset,
		A - Offset
	};

	// Reuse AppendQuad's triangulation; color carried via the vertex color.
	AppendQuad(Corners, RenderTransform, AtlasViewMin, LocalScale, VertexColor, OutVerts, OutIndices);
}


// -----------------------------------------------------------------------------
// AppendInstance - dispatch one building's computed geometry into the buffer.
// Reproduces the legacy per-type FCanvas draw (CartographGameInstanceModule.cpp
// :740-840) as custom-vert primitives:
//   Icon       -> textured quad built from ScreenPos (center) + Size + RotationDeg
//   Rectangle  -> filled quad from Corners[4] (+ 4 outline strokes if thick > 0)
//   Spline     -> N-1 thick-line segments along SplinePointsScreen
//   Wire/Beam  -> one thick line from ScreenPos to WireOrBeamEndScreen
// Color is baked into the vertex color; the shared resource supplies the texel
// for icons (SPIKE Q4 resource form + per-icon UV packing).
// -----------------------------------------------------------------------------
void SCartographMapView::AppendInstance(
	const FDrawGeometry& Geo,
	const FSlateRenderTransform& RenderTransform,
	const FVector2f& AtlasViewMin,
	const FVector2f& LocalScale,
	TArray<FSlateVertex>& OutVerts,
	TArray<SlateIndex>& OutIndices) const
{
	switch (Geo.Type)
	{
	case EBuildingDrawType::Icon:
	{
		// Per the FDrawGeometry contract, Icon fills ScreenPos (center), Size (px),
		// and RotationDeg - NOT Corners (Corners is Rectangle-only). Build the four
		// rotated corners about the center here (legacy FCanvasTileItem with
		// PivotPoint{0.5,0.5} + Rotation). Tint white so the icon texel shows
		// through (legacy FLinearColor::White).
		const float HalfW = Geo.Size.X * 0.5f;
		const float HalfH = Geo.Size.Y * 0.5f;
		const float Rad = FMath::DegreesToRadians(Geo.RotationDeg);
		const float CosR = FMath::Cos(Rad);
		const float SinR = FMath::Sin(Rad);

		auto RotateAboutCenter = [&](float LocalX, float LocalY) -> FVector2f
		{
			return FVector2f(
				Geo.ScreenPos.X + LocalX * CosR - LocalY * SinR,
				Geo.ScreenPos.Y + LocalX * SinR + LocalY * CosR);
		};

		// CW from top-left to match the FDrawGeometry::Corners convention.
		const FVector2f IconCorners[4] = {
			RotateAboutCenter(-HalfW, -HalfH),
			RotateAboutCenter(HalfW, -HalfH),
			RotateAboutCenter(HalfW, HalfH),
			RotateAboutCenter(-HalfW, HalfH)
		};

		const FColor White = FColor::White;
		AppendQuad(IconCorners, RenderTransform, AtlasViewMin, LocalScale, White, OutVerts, OutIndices);
		break;
	}

	case EBuildingDrawType::Rectangle:
	{
		// Filled footprint quad (legacy FCanvasTileItem with MainColor).
		const FColor FillColor = Geo.MainColor.ToFColor(/*bSRGB=*/true);
		AppendQuad(Geo.Corners, RenderTransform, AtlasViewMin, LocalScale, FillColor, OutVerts, OutIndices);

		// Outline strokes around the four edges (legacy draw_line x4) when the
		// class declares a non-zero outline thickness.
		if (Geo.OutlineThickness > 0.f)
		{
			const FColor OutlineColor = Geo.OutlineColor.ToFColor(/*bSRGB=*/true);
			AppendThickLine(Geo.Corners[0], Geo.Corners[1], Geo.OutlineThickness, RenderTransform, AtlasViewMin, LocalScale, OutlineColor, OutVerts, OutIndices);
			AppendThickLine(Geo.Corners[1], Geo.Corners[2], Geo.OutlineThickness, RenderTransform, AtlasViewMin, LocalScale, OutlineColor, OutVerts, OutIndices);
			AppendThickLine(Geo.Corners[2], Geo.Corners[3], Geo.OutlineThickness, RenderTransform, AtlasViewMin, LocalScale, OutlineColor, OutVerts, OutIndices);
			AppendThickLine(Geo.Corners[3], Geo.Corners[0], Geo.OutlineThickness, RenderTransform, AtlasViewMin, LocalScale, OutlineColor, OutVerts, OutIndices);
		}
		break;
	}

	case EBuildingDrawType::Spline:
	{
		const FColor StrokeColor = Geo.StrokeColor.ToFColor(/*bSRGB=*/true);
		const int32 Num = Geo.SplinePointsScreen.Num();
		for (int32 SegmentIdx = 0; SegmentIdx < Num - 1; ++SegmentIdx)
		{
			AppendThickLine(
				Geo.SplinePointsScreen[SegmentIdx],
				Geo.SplinePointsScreen[SegmentIdx + 1],
				Geo.StrokeThickness,
				RenderTransform,
				AtlasViewMin,
				LocalScale,
				StrokeColor,
				OutVerts,
				OutIndices);
		}
		break;
	}

	case EBuildingDrawType::Wire:
	case EBuildingDrawType::Beam:
	{
		const FColor StrokeColor = Geo.StrokeColor.ToFColor(/*bSRGB=*/true);
		AppendThickLine(
			Geo.ScreenPos,
			Geo.WireOrBeamEndScreen,
			Geo.StrokeThickness,
			RenderTransform,
			AtlasViewMin,
			LocalScale,
			StrokeColor,
			OutVerts,
			OutIndices);
		break;
	}

	case EBuildingDrawType::Invalid:
	default:
		break;
	}
}


// -----------------------------------------------------------------------------
// OnPaint - build + submit the per-instance buffer for the current viewport AoI.
// -----------------------------------------------------------------------------
int32 SCartographMapView::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	// CVar gate: this backend is inert unless explicitly selected. The default
	// TiledCanvas compositor remains the always-available fallback (SPEC 4.2/Q4).
	if (CartographConfig::GetRenderBackend() != ECartographRenderBackend::SlateInstanced)
	{
		return LayerId;
	}

	// Nothing to draw if the spine is unbound, buildings are hidden, the resource
	// is unset (SPIKE Q4: a single shared resource is required for one batch), or
	// the viewport world rect is empty/degenerate.
	if (!bShowBuildings || !Store || !Grid || !ZBands || !ClassTable || !CustomVertsBrush)
	{
		return LayerId;
	}
	if (!ViewportWorldRect.bIsValid || ViewportWorldRect.GetSize().IsNearlyZero())
	{
		return LayerId;
	}

	// -------------------------------------------------------------------------
	// Build the atlas-pixel -> widget-local transform from the viewport world
	// rect. WorldToScreen maps world cm to atlas-pixel space; the viewport rect's
	// corners give the atlas-pixel window we stretch to fill the widget.
	// -------------------------------------------------------------------------
	const FVector2f LocalSize = FVector2f(AllottedGeometry.GetLocalSize());
	if (LocalSize.X <= 0.f || LocalSize.Y <= 0.f)
	{
		return LayerId;
	}

	const FVector2D WorldMin = ViewportWorldRect.Min;
	const FVector2D WorldMax = ViewportWorldRect.Max;

	// Note: WorldToScreen is monotonic per-axis, so mapping the world rect's
	// min/max corners yields the atlas-pixel window directly.
	const FVector2f AtlasMinRaw = CartographCoords::WorldToScreen(FVector2D(WorldMin.X, WorldMin.Y));
	const FVector2f AtlasMaxRaw = CartographCoords::WorldToScreen(FVector2D(WorldMax.X, WorldMax.Y));
	const FVector2f AtlasViewMin(FMath::Min(AtlasMinRaw.X, AtlasMaxRaw.X), FMath::Min(AtlasMinRaw.Y, AtlasMaxRaw.Y));
	const FVector2f AtlasViewMax(FMath::Max(AtlasMinRaw.X, AtlasMaxRaw.X), FMath::Max(AtlasMinRaw.Y, AtlasMaxRaw.Y));

	const FVector2f AtlasSpan = AtlasViewMax - AtlasViewMin;
	if (AtlasSpan.X <= KINDA_SMALL_NUMBER || AtlasSpan.Y <= KINDA_SMALL_NUMBER)
	{
		return LayerId;
	}

	// Local px per atlas px (the zoom factor on each axis).
	const FVector2f LocalScale(LocalSize.X / AtlasSpan.X, LocalSize.Y / AtlasSpan.Y);

	// Widget local Slate space -> window space. Baked into each FSlateVertex so
	// the single batch lands at the panel's on-screen position/scale.
	const FSlateRenderTransform RenderTransform = AllottedGeometry.GetAccumulatedRenderTransform();

	// AoI cull box in WORLD cm, expanded by BOX_EXPANSION so a building whose
	// anchor is just off-screen but whose footprint reaches in is still gathered
	// (the grid query is cell-granular + the per-record VisualBox refines below).
	const FBox2f WorldQueryBox(
		FVector2f((float)WorldMin.X - BOX_EXPANSION_CENTIMETERS, (float)WorldMin.Y - BOX_EXPANSION_CENTIMETERS),
		FVector2f((float)WorldMax.X + BOX_EXPANSION_CENTIMETERS, (float)WorldMax.Y + BOX_EXPANSION_CENTIMETERS));

	// Screen-space (atlas-pixel) cull box used to reject geometry whose VisualBox
	// does not touch the visible window (the false-positive refine for the
	// cell-granular grid query). Expanded by half a tile so thick strokes that
	// straddle the edge are not clipped early.
	const FBox2f ScreenCullBox(AtlasViewMin, AtlasViewMax);

	// -------------------------------------------------------------------------
	// Per-paint instance buffer (rebuilt each paint, never cached on the widget).
	// Reserve against a coarse upper bound on AoI occupancy to avoid realloc
	// churn; the grid query gives the actual handle count.
	// -------------------------------------------------------------------------
	TArray<FSlateVertex> Verts;
	TArray<SlateIndex> Indices;

	// De-dup multi-cell handles (the grid query can return a handle once per
	// covered cell). A small set keyed by the generational handle is ABA-safe.
	// We use the TArray (non-template) overloads of QueryRect / QueryBandRange so
	// the visitor template definitions (which live in the owning agents' .cpp,
	// not their headers) are never instantiated across this TU.
	TArray<FBuildingHandle> AoIHandles;
	Grid->QueryRect(WorldQueryBox, AoIHandles);
	if (AoIHandles.Num() == 0)
	{
		return LayerId;  // nothing in view
	}

	TSet<FBuildingHandle> Seen;
	Seen.Reserve(AoIHandles.Num());
	for (const FBuildingHandle& Handle : AoIHandles)
	{
		Seen.Add(Handle);
	}
	Verts.Reserve(Seen.Num() * 4);
	Indices.Reserve(Seen.Num() * 6);

	// -------------------------------------------------------------------------
	// Walk the Z-band index low->high (back-to-front). QueryBandRange appends in
	// low-band-first order, so iterating ZBandHandles in order IS painter's
	// order: the instance order bakes the z-order (MakeCustomVerts is not
	// depth-sorted) - SPEC 4.2. For each handle in the Z-filter range that is
	// ALSO in the AoI grid set, compute geometry and append.
	// -------------------------------------------------------------------------
	TArray<FBuildingHandle> ZBandHandles;
	ZBands->QueryBandRange(ZFilterMin, ZFilterMax, ZBandHandles);

	int32 DrawnCount = 0;
	for (const FBuildingHandle& Handle : ZBandHandles)
	{
		// Must be both in the Z-filter band range (it is) AND in the viewport AoI
		// (the grid set). Erase-on-hit so a handle that appeared in multiple grid
		// cells is drawn exactly once.
		if (Seen.Remove(Handle) == 0)
		{
			continue;  // not in AoI (or already drawn)
		}

		const FBuildingRecord* Record = Store->Find(Handle);
		if (!Record)
		{
			continue;  // stale handle (raced removal) - skip, never deref
		}

		const FClassDrawInfo* Info = ClassTable->FindInfo(Record->ClassId);
		if (!Info)
		{
			continue;  // unregistered class
		}

		FDrawGeometry Geo;
		if (!FCartographClassDrawTable::ComputeDrawGeometry(*Record, *Info, *Store, Geo))
		{
			continue;  // not drawable (zero footprint / zero stroke) - legacy early-out
		}

		// Screen-space AoI refine: skip geometry whose VisualBox does not touch
		// the visible atlas-pixel window (the grid query was cell-granular).
		if (Geo.VisualBox.bIsValid && !Geo.VisualBox.Intersect(ScreenCullBox))
		{
			continue;
		}

		AppendInstance(Geo, RenderTransform, AtlasViewMin, LocalScale, Verts, Indices);
		++DrawnCount;
	}

	if (Verts.Num() == 0 || Indices.Num() == 0)
	{
		return LayerId;
	}

	CARTO_LOG_VERY_VERBOSE("SlateInstanced OnPaint: %d drawn, %d verts, %d indices", DrawnCount, Verts.Num(), Indices.Num());

	// -------------------------------------------------------------------------
	// ONE instanced draw for the whole viewport. The shared resource (icons
	// packed into the shared atlas/material) is the brush handle.
	// SPIKE(Q4): FSlateDrawElement::MakeCustomVerts symbol-export + its exact
	// signature (paint geometry, resource handle, verts, indices, [instance
	// data]) and UMG/HUD compositing are unverified on the CSS Slate build.
	// -------------------------------------------------------------------------
	const FSlateResourceHandle ResourceHandle =
		FSlateApplication::Get().GetRenderer()->GetResourceHandle(*CustomVertsBrush);

	// SPIKE(Q4): GetResourceHandle returns an INVALID handle unless CustomVertsBrush is
	// a registered / atlas-resident resource. MakeCustomVerts silently no-ops on an
	// invalid handle, so guard here: an invalid handle means the brush was not yet
	// registered with the renderer (the brush MUST be a real atlas resource for the
	// CSS Slate build to resolve it). Bail rather than emit a no-op draw element.
	if (!ResourceHandle.IsValid())
	{
		return LayerId;
	}

	FSlateDrawElement::MakeCustomVerts(
		OutDrawElements,
		LayerId,
		ResourceHandle,
		Verts,
		Indices,
		/*InInstanceData=*/nullptr,
		/*InInstanceOffset=*/0,
		/*InNumInstances=*/0);

	return LayerId + 1;
}

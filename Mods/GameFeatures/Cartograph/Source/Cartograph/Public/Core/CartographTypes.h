#pragma once

#include "CoreMinimal.h"
#include "CartographConfig.h"

// =============================================================================
// CartographTypes.h - FROZEN SHARED TYPES (Agent: contracts)
// -----------------------------------------------------------------------------
// Pure data + tiny inline helpers shared by EVERY subsystem and render backend.
// No behavior beyond trivial accessors and coordinate math. Signatures here are
// FROZEN: downstream agents (store, grid, z-band, class-table, compositor,
// replicator, slate/instance renderers) all compile against these and must not
// change them.
//
// These are plain C++ structs (no UCLASS/USTRUCT/GENERATED_BODY) unless a type
// crosses the reflection boundary; none here does. Repo style: PascalCase
// locals, Allman braces, tabs, English comments.
// =============================================================================


class UTexture2D;


// -----------------------------------------------------------------------------
// FBuildingHandle - generational, ABA-safe stable identity (SPEC 4.1).
// NEVER an array index. Slot indexes into the SoA store's TSparseArray;
// Gen disambiguates a recycled slot so a stale network delta or UI pick can
// never alias a different building after slot reuse.
// -----------------------------------------------------------------------------
struct FBuildingHandle
{
	uint32 Slot = INDEX_NONE;
	uint16 Gen = 0;

	FBuildingHandle() = default;
	FBuildingHandle(uint32 InSlot, uint16 InGen)
		: Slot(InSlot)
		, Gen(InGen)
	{
	}

	/** A handle is valid iff its slot is set. Validity against the store (live
	 *  slot + matching Gen) is checked by FCartographBuildingStore::IsValid. */
	FORCEINLINE bool IsValid() const
	{
		return Slot != (uint32)INDEX_NONE;
	}

	FORCEINLINE bool operator==(const FBuildingHandle& Other) const
	{
		return Slot == Other.Slot && Gen == Other.Gen;
	}

	FORCEINLINE bool operator!=(const FBuildingHandle& Other) const
	{
		return !(*this == Other);
	}
};

/** Hash includes Gen so recycled slots hash distinctly; usable as TMap/TSet key. */
FORCEINLINE uint32 GetTypeHash(const FBuildingHandle& Handle)
{
	return HashCombine(::GetTypeHash(Handle.Slot), ::GetTypeHash(Handle.Gen));
}


// -----------------------------------------------------------------------------
// EBuildingDrawType - how a building is rasterized (SPEC 4.1).
// Replaces the legacy bit-flag EBuildingDataType. Single value per record (a
// building is exactly one draw type); typed side-tables hold the extra data.
// -----------------------------------------------------------------------------
enum class EBuildingDrawType : uint8
{
	Invalid = 0,
	Icon = 1,       // textured quad
	Rectangle = 2,  // filled+outlined footprint quad
	Spline = 3,     // belt / pipe / rail polyline
	Wire = 4,       // power line segment
	Beam = 5,       // beam (start + direction*length)
};


// -----------------------------------------------------------------------------
// FClassDrawInfo - per-class draw data, keyed by ClassId (SPEC 4.1 "per-class
// table"). Sized by distinct classes (hundreds), built once at gather. Collapses
// the four per-building pointers the legacy design stored (Category/Layer/Spline/
// Wire) into one hash lookup per class at draw time.
//
// Colors/thickness mirror the legacy FCategoryData/FSplineData/FWireData fields
// so ComputeDrawGeometry reproduces the old appearance pixel-identically.
// -----------------------------------------------------------------------------
struct FClassDrawInfo
{
	/** Draw primitive for this class. */
	EBuildingDrawType DrawType = EBuildingDrawType::Invalid;

	/** Icon texture (Icon draw type). Soft so the table is cheap; PIN before a
	 *  draw pass so the compositor never AsyncLoads mid-pass (SPEC 4.3). */
	TSoftObjectPtr<UTexture2D> Icon;

	/** Unscaled footprint size in cm (Rectangle/Icon). Legacy GetBuildingSize.
	 *  Per-instance scale is applied at draw from the record's transform. */
	FVector2f Footprint = FVector2f::ZeroVector;

	/** Filled-rectangle main fill color (legacy FCategoryData::MainColor). */
	FLinearColor MainColor = FLinearColor::White;

	/** Rectangle outline color (legacy FCategoryData::OutlineColor). */
	FLinearColor OutlineColor = FLinearColor::Black;

	/** Rectangle outline thickness in px (legacy FCategoryData::OutlineThickness). */
	float OutlineThickness = 0.f;

	/** Spline/Wire/Beam stroke color (legacy FSplineData/FWireData::Color). */
	float StrokeThickness = 0.f;
	FLinearColor StrokeColor = FLinearColor::White;

	/** Extra rotation applied on top of the transform yaw (legacy
	 *  BuildableExtraRotationMap), degrees, yaw only for top-down. */
	float ExtraYawDegrees = 0.f;

	/** Layer identity for filter/toggle (legacy FBuildLayerData main/sub). */
	FName LayerMainCategory = NAME_None;
	FName LayerSubCategory = NAME_None;
};


// -----------------------------------------------------------------------------
// Typed side-tables (SPEC 4.1). A belt never pays for a rectangle cache: the
// record's ExtraIndex points into exactly the table matching its draw type.
// Stored single-precision (FVector2f) - top-down 2D, matches the wire format.
// -----------------------------------------------------------------------------

/** Spline polyline in world cm (XY). Mirrors legacy FSplineExtraData::Points
 *  but single-precision. SPLINE_SEGMENTS+1 points sampled along the spline. */
struct FSplineExtra
{
	TArray<FVector2f> Points;
};

/** Wire end point in world cm (XY). Mirrors legacy FWireExtraData::End.
 *  The start is the record's Pos.XY. */
struct FWireExtra
{
	FVector2f End = FVector2f::ZeroVector;
};

/** Beam length in cm. Mirrors legacy FBeamExtraData::Length.
 *  Direction comes from the record's PackedYaw. */
struct FBeamExtra
{
	float Length = 0.f;
};


// -----------------------------------------------------------------------------
// FBuildingRecord - the slim per-building record (SPEC 4.1 table).
// ~24-28 B vs legacy ~344-432 B. No precomputed draw cache, no scale, no quat -
// all recomputed at draw via FCartographClassDrawTable::ComputeDrawGeometry.
//
//   Pos        FVector3f  12 B   single-precision sufficient (~0.05 cm worst error)
//   PackedYaw  uint16      2 B   quantized yaw; only yaw is needed for top-down
//   ClassId    uint16      2 B   index into the ClassDrawTable
//   Type       uint8       1 B   EBuildingDrawType
//   ExtraIndex uint32      4 B   index into the typed side-table for Type
//                                (INDEX_NONE / no extra for Icon/Rectangle)
// -----------------------------------------------------------------------------
struct FBuildingRecord
{
	FVector3f Pos = FVector3f::ZeroVector;
	uint16 PackedYaw = 0;
	uint16 ClassId = 0;
	EBuildingDrawType Type = EBuildingDrawType::Invalid;
	uint32 ExtraIndex = (uint32)INDEX_NONE;

	/** Decode PackedYaw back to degrees [0, 360). Inverse of PackYaw. */
	FORCEINLINE float GetYawDegrees() const
	{
		return (float)PackedYaw * (360.f / 65536.f);
	}

	/** Quantize a yaw in degrees to the uint16 wire form (wraps to [0, 360)). */
	static FORCEINLINE uint16 PackYaw(float YawDegrees)
	{
		float Wrapped = FMath::Fmod(YawDegrees, 360.f);
		if (Wrapped < 0.f)
		{
			Wrapped += 360.f;
		}
		return (uint16)FMath::RoundToInt(Wrapped * (65536.f / 360.f)) & 0xFFFF;
	}
};


// -----------------------------------------------------------------------------
// FDrawGeometry - output of ComputeDrawGeometry, consumed by ALL render
// backends (FCanvas / Slate verts / instance custom-data). Recomputed per draw;
// never stored on the record. Reproduces the legacy FillInCache results.
//
// Which fields are meaningful depends on Type:
//   Icon       -> ScreenPos, Size, RotationDeg, Icon
//   Rectangle  -> ScreenPos, Size, RotationDeg, Corners[4] (screen space), colors
//   Spline     -> SplinePointsScreen, StrokeThickness, StrokeColor
//   Wire       -> ScreenPos (start), WireEndScreen, StrokeThickness, StrokeColor
//   Beam       -> ScreenPos (start), BeamEndScreen, StrokeThickness, StrokeColor
// VisualBox is always set (screen-space, expanded by BOX_EXPANSION) and is what
// the tile/grid dirty-rect bookkeeping keys on.
// -----------------------------------------------------------------------------
struct FDrawGeometry
{
	EBuildingDrawType Type = EBuildingDrawType::Invalid;

	/** Screen-space anchor (Icon/Rectangle center; Wire/Beam start). Px. */
	FVector2f ScreenPos = FVector2f::ZeroVector;

	/** Screen-space size (Icon/Rectangle). Px. */
	FVector2f Size = FVector2f::ZeroVector;

	/** Final yaw in degrees (transform yaw + class ExtraYaw). */
	float RotationDeg = 0.f;

	/** Rectangle corners in screen space, CW from top-left. Valid for Rectangle. */
	FVector2f Corners[4] = { FVector2f::ZeroVector, FVector2f::ZeroVector,
							 FVector2f::ZeroVector, FVector2f::ZeroVector };

	/** Spline polyline in screen space (Spline). */
	TArray<FVector2f> SplinePointsScreen;

	/** Wire/Beam end point in screen space (Wire/Beam). */
	FVector2f WireOrBeamEndScreen = FVector2f::ZeroVector;

	/** Resolved icon texture (Icon), already pinned by the caller. */
	TSoftObjectPtr<UTexture2D> Icon;

	/** Fill/outline (Rectangle), stroke (Spline/Wire/Beam). */
	FLinearColor MainColor = FLinearColor::White;
	FLinearColor OutlineColor = FLinearColor::Black;
	float OutlineThickness = 0.f;
	float StrokeThickness = 0.f;
	FLinearColor StrokeColor = FLinearColor::White;

	/** Screen-space bounds, expanded by BOX_EXPANSION_CENTIMETERS-equivalent.
	 *  Drives which tiles/cells this building marks dirty. Empty if not drawn. */
	FBox2f VisualBox = FBox2f(ForceInit);
};


// -----------------------------------------------------------------------------
// Tile / version typedefs (SPEC 4.2, 4.4).
//   FTileId      - flat tile index in [0, TILE_COUNT). Y * TILES_PER_AXIS + X.
//   FTileVersion - monotonic per-tile uint64 counter, O(1) bump on dirty
//                  (SPEC Q8: counter, NOT a content hash on the hot path).
// -----------------------------------------------------------------------------
using FTileId = uint32;
using FTileVersion = uint64;

/** Sentinel for "no tile". */
inline constexpr FTileId InvalidTileId = (FTileId)INDEX_NONE;


// =============================================================================
// Coordinate helpers (FROZEN). Ported from legacy world<->screen math so the
// rendered map is pixel-identical (SPEC Q12). All screen positions are in the
// RENDER_TEXTURE_SIZE pixel space of the persistent atlas.
// =============================================================================
namespace CartographCoords
{
	/**
	 * World position (centimeters) -> screen position (pixels), accounting for a
	 * building's footprint size so the anchor is the top-left of the centered
	 * footprint. Ports legacy world_position_to_screen_position.
	 * Generic over FVector / FVector2D / FVector3f / FVector2f (X,Y members).
	 */
	template<typename TPos, typename TSize>
	FORCEINLINE FVector2f WorldToScreen(const TPos& WorldPosition, const TSize& Size)
	{
		return FVector2f{
			(float)((ORIGIN_UV[0] + ((double)WorldPosition.X - (double)Size.X / 2.0) / MAP_WIDTH_CENTIMETERS) * RENDER_TEXTURE_SIZE),
			(float)((ORIGIN_UV[1] + ((double)WorldPosition.Y - (double)Size.Y / 2.0) / MAP_HEIGHT_CENTIMETERS) * RENDER_TEXTURE_SIZE)
		};
	}

	/** Overload for a point with no footprint offset (Size == 0). */
	template<typename TPos>
	FORCEINLINE FVector2f WorldToScreen(const TPos& WorldPosition)
	{
		return FVector2f{
			(float)((ORIGIN_UV[0] + (double)WorldPosition.X / MAP_WIDTH_CENTIMETERS) * RENDER_TEXTURE_SIZE),
			(float)((ORIGIN_UV[1] + (double)WorldPosition.Y / MAP_HEIGHT_CENTIMETERS) * RENDER_TEXTURE_SIZE)
		};
	}

	/** Screen position (pixels) -> world position (centimeters, XY).
	 *  Ports legacy screen_position_to_world_position. */
	template<typename TPos>
	FORCEINLINE FVector2D ScreenToWorld(const TPos& ScreenPosition)
	{
		return FVector2D{
			((double)ScreenPosition.X / RENDER_TEXTURE_SIZE - ORIGIN_UV[0]) * MAP_WIDTH_CENTIMETERS,
			((double)ScreenPosition.Y / RENDER_TEXTURE_SIZE - ORIGIN_UV[1]) * MAP_HEIGHT_CENTIMETERS
		};
	}

	/** Build a flat tile id from tile-grid X,Y. No bounds check. */
	FORCEINLINE FTileId TileId(int32 TileX, int32 TileY)
	{
		return (FTileId)(TileY * TILES_PER_AXIS + TileX);
	}

	/** Decompose a flat tile id into tile-grid X,Y. */
	FORCEINLINE void TileXY(FTileId Tile, int32& OutX, int32& OutY)
	{
		OutX = (int32)(Tile % TILES_PER_AXIS);
		OutY = (int32)(Tile / TILES_PER_AXIS);
	}

	/** Screen position (pixels) -> tile id. Clamped into [0, TILE_COUNT). */
	FORCEINLINE FTileId TileIdFromScreen(const FVector2f& ScreenPos)
	{
		const int32 TileX = FMath::Clamp((int32)(ScreenPos.X / TILE_SIZE_PX), 0, TILES_PER_AXIS - 1);
		const int32 TileY = FMath::Clamp((int32)(ScreenPos.Y / TILE_SIZE_PX), 0, TILES_PER_AXIS - 1);
		return TileId(TileX, TileY);
	}

	/** Tile id -> its pixel rect in the atlas. The bottom/right edge tiles are
	 *  clamped to RENDER_TEXTURE_SIZE so ragged tiles do not overrun the atlas. */
	FORCEINLINE FBox2f ScreenRectFromTile(FTileId Tile)
	{
		int32 TileX, TileY;
		TileXY(Tile, TileX, TileY);
		const float MinX = (float)(TileX * TILE_SIZE_PX);
		const float MinY = (float)(TileY * TILE_SIZE_PX);
		const float MaxX = FMath::Min((float)((TileX + 1) * TILE_SIZE_PX), (float)RENDER_TEXTURE_SIZE);
		const float MaxY = FMath::Min((float)((TileY + 1) * TILE_SIZE_PX), (float)RENDER_TEXTURE_SIZE);
		return FBox2f(FVector2f(MinX, MinY), FVector2f(MaxX, MaxY));
	}

	/** World position (cm, XY) -> spatial-grid cell coordinates (clamped). */
	FORCEINLINE FIntPoint GridCellFromWorld(double WorldX, double WorldY)
	{
		const int32 CellX = FMath::Clamp((int32)((WorldX - WEST_BOUND_CENTIMETERS) / GRID_CELL_SIZE_CM), 0, GRID_CELLS_X - 1);
		const int32 CellY = FMath::Clamp((int32)((WorldY - NORTH_BOUND_CENTIMETERS) / GRID_CELL_SIZE_CM), 0, GRID_CELLS_Y - 1);
		return FIntPoint(CellX, CellY);
	}

	/** Z (cm) -> Z-band index, clamped into [0, Z_BAND_COUNT). */
	FORCEINLINE int32 ZBandFromWorldZ(float WorldZ)
	{
		return FMath::Clamp((int32)(((double)WorldZ - Z_BAND_MIN_CM) / Z_BAND_HEIGHT_CM), 0, Z_BAND_COUNT - 1);
	}
}
